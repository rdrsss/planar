// @file update_leaves.t.cpp
// @brief Tests for `planar update` and its native mutation-lock port (plan
// 1122 M3, task rel-update-verb; tech spec 677, "The update verb" and
// "Mutation ownership and cleanup"; test spec 679).
//
// Three layers, each for what only it can show:
//
//   - The lock port against the SHELL library it must interoperate with:
//     every case drives `scripts/install-lib/mutation-lock.sh` in real bash
//     processes beside the native code, so a record either side writes is
//     judged the same way by the other.
//   - The verb in-process with an injected host (`update_host`): the platform,
//     the `ldd` line and the exec are the test's, so the refusals, the
//     hand-off request and the cleanup are asserted on every host the suite
//     runs on, including ones no bundle exists for.
//   - The built binary, black-box, with the real `install.sh --prebuilt`
//     behind the exec, a `bash` stand-in that inspects the lock from inside
//     the exec'd process, and a KILL in the middle of a download.
//
// Every release is a fake one (`release_fixture.sh`, which builds on
// `scripts/fixtures/prebuilt-bundle.sh`) served through `file://` or the
// in-process loopback HTTP fixture. HOME, PLANAR_HOME and PLANAR_DB always
// point into a scratch arena; nothing here reaches the operator's ~/.planar.

#include <catch2/catch_test_macros.hpp>

#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import cli11;
import planar.cliapp.args;
import planar.cmd.planar.context;
import planar.cmd.planar.dispatch;
import planar.cmd.planar.handler;
import planar.cmd.planar.main;
import planar.cmd.planar.handlers.update;
import planar.cmd.planar.handlers.update.lock;
import planar.cmd.planar.handlers.update.state;
import planar.db.migrate;

#include "../lib/http/fixture_server.hpp"
#include "parity_harness.hpp"

namespace {

namespace up   = planar::cmd::update;
namespace lock = planar::cmd::update::lock;
using planar::cmd::context;
using planar::cmd::handlers::exec_request;
using planar::cmd::handlers::update_host;
using planar::cmd::parity::make_arena;
using planar::cmd::parity::pinned_env;
using planar::cmd::parity::pinned_var;
using planar::cmd::parity::read_all;
using planar::cmd::parity::run_pinned;
using planar::cmd::parity::shell_quote;

auto source_root() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_TARGET_SOURCE_ROOT};
}

auto lock_lib() -> std::string {
  return (source_root() / "scripts/install-lib/mutation-lock.sh").string();
}

/// Run `script` under bash with `args`; stdout+stderr into `out`.
auto bash(const std::string& script, const std::vector<std::string>& args, std::string* out = nullptr) -> int {
  auto const  capture = std::filesystem::temp_directory_path() / std::format("planar-update-bash-{}-{}", ::getpid(), std::rand());
  std::string line    = "bash -c " + shell_quote(script) + " bash";
  for (auto const& a : args) {
    line += " " + shell_quote(a);
  }
  line += " > " + shell_quote(capture.string()) + " 2>&1";
  int const rc = std::system(line.c_str());
  if (out != nullptr) {
    *out = read_all(capture);
  }
  std::error_code ec;
  std::filesystem::remove(capture, ec);
  return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

auto write_file(const std::filesystem::path& path, std::string_view body) -> void {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file << body;
}

auto canon(const std::filesystem::path& p) -> std::string {
  return up::canonical_path(p.string(), "/").value();
}

/// A record file the shell library would accept, with every field chosen.
auto record_text(std::string_view gen, std::string_view op, std::string_view pid, std::string_view start, std::string_view node,
                 std::string_view root, std::string_view tmp = "") -> std::string {
  return std::format("planar-mutation-lock 1\ngen={}\noperation={}\npid={}\nstart={}\nnode={}\nnonce={}\nroot={}\ntmp={}\n", gen,
                     op, pid, start, node, std::string(32, 'a'), root, tmp);
}

/// A pid no process has: far above any pid_max on the supported platforms.
constexpr std::string_view k_dead_pid = "99999999";

auto highest_gen(const std::string& dir) -> std::string {
  std::uint64_t   best = 0;
  std::error_code ec;
  for (auto const& e : std::filesystem::directory_iterator(dir, ec)) {
    auto const name = e.path().filename().string();
    if (name.starts_with("owner.")) {
      best = std::max<std::uint64_t>(best, std::stoull(name.substr(6)));
    }
  }
  return std::to_string(best);
}

/// No live owner: no owner record, or the highest one released.
auto lock_free(const std::string& dir) -> bool {
  auto const g = highest_gen(dir);
  return g == "0" || std::filesystem::exists(std::format("{}/released.{}", dir, g));
}

// ---------------------------------------------------------------------------
// The verb in-process, with an injected host.
// ---------------------------------------------------------------------------

struct world {
  planar::cmd::parity::arena space;
  std::filesystem::path      root; // the arena
  std::filesystem::path      home; // $HOME
  std::filesystem::path      inst; // $HOME/.planar, the install root
  std::filesystem::path      rel;  // the file:// release base
  std::string                base; // PLANAR_RELEASE_URL
};

auto make_world(std::string_view tag) -> world {
  world w{.space = make_arena(tag)};
  w.root = w.space.cpp_root;
  w.home = w.root / "fakehome";
  w.inst = w.home / ".planar";
  w.rel  = w.root / "rel";
  w.base = "file://" + canon(w.rel);
  std::filesystem::create_directories(w.rel);
  return w;
}

/// Publish a fake release under `w.rel`.
auto publish(const world& w, std::string_view tag, std::string_view platform, int schema = 41, bool latest = true) -> void {
  std::string out;
  auto const  rc = bash(R"("$1/src/cmd/planar/handlers/update/release_fixture.sh" "$@")",
                        {source_root().string(), w.rel.string(), std::string{tag}, std::string{platform}, std::to_string(schema),
                         latest ? "yes" : "no"},
                        &out);
  INFO(out);
  REQUIRE(rc == 0);
}

/// A completed install of `tag`, as far as the verb reads one.
auto seed_install(const world& w, std::string_view tag) -> void {
  write_file(w.inst / "release.json", std::format("{{\n  \"version\": \"{}\",\n  \"schema_version\": 41\n}}\n", tag));
  write_file(w.inst / ".planar-install", "installer\nbuild\n");
  write_file(w.inst / "bin/planar", "#!/bin/sh\n");
}

struct recorder {
  std::optional<exec_request> request;
  std::expected<void, int>    result;
};

auto host_for(std::string platform, std::optional<std::string> ldd, recorder& rec) -> update_host {
  return update_host{
      .platform       = [platform]() -> std::expected<std::string, std::string> { return platform; },
      .ldd_first_line = [ldd] { return ldd; },
      .exec           = [&rec](const exec_request& req) -> std::expected<void, int> {
        rec.request = req;
        return rec.result;
      },
  };
}

struct invocation {
  int         code = -1;
  std::string out;
  std::string err;
  bool        db_open = false;
};

auto invoke(const world& w, std::vector<std::string> args, const update_host& host,
            std::map<std::string, std::string, std::less<>> extra = {}) -> invocation {
  std::map<std::string, std::string, std::less<>> vars{
      {"HOME", w.home.string()},
      {"PLANAR_DB", (w.root / "planar.db").string()},
      {"PLANAR_RELEASE_URL", w.base},
      {"PATH", std::getenv("PATH") != nullptr ? std::getenv("PATH") : "/usr/bin:/bin"},
  };
  for (auto& [k, v] : extra) {
    vars[k] = v;
  }
  std::vector<std::string> argv{"planar", "update"};
  argv.insert(argv.end(), args.begin(), args.end());
  std::ostringstream out;
  std::ostringstream err;
  context            ctx{std::move(argv),
                         planar::cmd::map_env(vars),
                         w.root / "proj",
                         std::make_shared<planar::cmd::database>(w.root / "planar.db", err),
                         out,
                         err};
  auto const         tree  = planar::cmd::root_app();
  auto               table = planar::cmd::make_handler_table(*tree);
  table["update"]          = [&host](context& c, const planar::cliapp::parsed_args& a) {
    return planar::cmd::handlers::run_update(c, a, host);
  };
  int const code = planar::cmd::run(ctx, *tree, table);
  return {.code = code, .out = out.str(), .err = err.str(), .db_open = ctx.db().opened()};
}

auto update_ns(const world& w) -> std::filesystem::path {
  return w.inst / ".planar-update";
}

} // namespace

TEST_CASE("update: --check prints installed and latest and exits 10 when they differ, 0 when equal", "[update]") {
  auto w = make_world("upcheck");
  publish(w, "v1.0.0", "macos-arm64");
  publish(w, "v1.1.0", "macos-arm64");
  seed_install(w, "v1.0.0");
  recorder   rec;
  auto const host = host_for("macos-arm64", std::nullopt, rec);

  auto const newer = invoke(w, {"--check"}, host);
  CHECK(newer.code == 10);
  CHECK(newer.out == "installed v1.0.0 latest v1.1.0\n");
  CHECK(newer.err.empty());
  CHECK_FALSE(newer.db_open);

  write_file(w.rel / "latest/download/VERSION", "v1.0.0\n");
  auto const same = invoke(w, {"--check"}, host);
  CHECK(same.code == 0);
  CHECK(same.out == "installed v1.0.0 latest v1.0.0\n");

  // A missing release directory is an unreachable server: a fault, exit 1.
  auto const gone = invoke(w, {"--check"}, host, {{"PLANAR_RELEASE_URL", "file://" + canon(w.root / "nowhere")}});
  CHECK(gone.code == 1);
  CHECK(gone.err.starts_with("error: cannot reach the release server at file://"));
  // --check takes no lock and changes nothing.
  CHECK_FALSE(std::filesystem::exists(canon(w.inst) + ".lock"));
  CHECK_FALSE(rec.request.has_value());
}

TEST_CASE("update: --check with no release.json reports installed none and exits 10", "[update]") {
  auto w = make_world("upnone");
  publish(w, "v1.1.0", "macos-arm64");
  recorder   rec;
  auto const got = invoke(w, {"--check"}, host_for("macos-arm64", std::nullopt, rec));
  CHECK(got.code == 10);
  CHECK(got.out == "installed none latest v1.1.0\n");
}

TEST_CASE("update: --check and --version are input errors when combined or malformed", "[update]") {
  auto       w = make_world("upinput");
  recorder   rec;
  auto const host = host_for("macos-arm64", std::nullopt, rec);
  auto const both = invoke(w, {"--check", "--version", "v1.0.0"}, host);
  CHECK(both.code == 2);
  auto const bad = invoke(w, {"--version", "1.0"}, host);
  CHECK(bad.code == 2);
  CHECK(bad.err.contains("is not a release tag"));
  auto const base = invoke(w, {"--check"}, host, {{"PLANAR_RELEASE_URL", "http://localhost.example/releases"}});
  CHECK(base.code == 2);
  CHECK(base.err.starts_with("error: PLANAR_RELEASE_URL='http://localhost.example/releases' is not an accepted release base"));
}

TEST_CASE("update: a plain run hands its live ownership to the installer it execs", "[update]") {
  auto w = make_world("uphandoff");
  publish(w, "v1.0.0", "macos-arm64");
  publish(w, "v1.1.0", "macos-arm64");
  seed_install(w, "v1.0.0");
  recorder   rec;
  auto const got = invoke(w, {}, host_for("macos-arm64", std::nullopt, rec));
  INFO(got.out << got.err);
  REQUIRE(got.code == 0);
  REQUIRE(rec.request.has_value());
  CHECK_FALSE(got.db_open);

  auto const root     = canon(w.inst);
  auto const dir      = root + ".lock";
  auto const g        = highest_gen(dir);
  auto const rec_file = lock::parse_record(std::format("{}/owner.{}", dir, g));
  REQUIRE(rec_file.has_value());
  // Ownership was never released before the exec: the record is the highest,
  // unreleased, unconsumed, names this process and an update.
  CHECK_FALSE(std::filesystem::exists(std::format("{}/released.{}", dir, g)));
  CHECK_FALSE(std::filesystem::exists(std::format("{}/handoff.{}", dir, g)));
  CHECK(rec_file->operation == "update");
  CHECK(rec_file->pid == std::to_string(::getpid()));
  CHECK(rec_file->root == root);
  CHECK(lock::start_token(::getpid()) == rec_file->start);

  auto const& req    = *rec.request;
  auto const  tmp    = rec_file->tmp;
  auto const  bundle = tmp + "/x/planar-macos-arm64";
  CHECK(tmp.starts_with(root + "/.planar-update/update-"));
  CHECK(lock::update_tmp_valid(root, tmp));
  CHECK(std::filesystem::path{req.program}.filename() == "bash");
  CHECK(req.argv == std::vector<std::string>{"bash", bundle + "/install.sh", "--prebuilt", bundle, "--cleanup", tmp});
  std::map<std::string, std::optional<std::string>> env(req.env.begin(), req.env.end());
  CHECK(env.at("PLANAR_MUTATION_HANDOFF") == std::format("{}:{}", g, rec_file->nonce));
  CHECK(env.at("PLANAR_RELEASE_URL") == w.base);
  CHECK_FALSE(env.at("PLANAR_EXPECT_RECOVERY").has_value());
  CHECK(std::filesystem::is_regular_file(bundle + "/install.sh"));
  CHECK(read_all(bundle + "/release.json").contains("\"version\": \"v1.1.0\""));

  // The shell library agrees that this process holds the lock: a shell
  // install refuses, naming this updater.
  std::string out;
  CHECK(bash(R"(source "$1"; planar_lock_acquire "$2" install || { echo "$PLANAR_LOCK_ERROR"; exit 1; })", {lock_lib(), root},
             &out) == 1);
  CHECK(out.contains(std::format("another Planar update (pid {})", ::getpid())));
  lock::release(lock::ownership{.dir = dir, .root = root, .gen = g, .nonce = rec_file->nonce});
}

TEST_CASE("update: a completed install of the resolved release reports no changes", "[update]") {
  auto w = make_world("upcurrent");
  publish(w, "v1.1.0", "macos-arm64");
  seed_install(w, "v1.1.0");
  recorder   rec;
  auto const host = host_for("macos-arm64", std::nullopt, rec);
  auto const got  = invoke(w, {}, host);
  CHECK(got.code == 0);
  CHECK(got.out.contains("Planar v1.1.0 is installed in"));
  CHECK(got.out.contains("no changes"));
  CHECK_FALSE(rec.request.has_value());
  CHECK_FALSE(std::filesystem::exists(update_ns(w)));
  CHECK(lock_free(canon(w.inst) + ".lock"));
  auto const pinned = invoke(w, {"--version", "v1.1.0"}, host);
  CHECK(pinned.code == 0);
  CHECK(pinned.out.contains("no changes"));
}

TEST_CASE("update: each refused fault leaves no temporary directory, no held lock and no change", "[update]") {
  auto w = make_world("upfaults");
  publish(w, "v1.0.0", "macos-arm64");
  publish(w, "v1.1.0", "macos-arm64");
  publish(w, "v1.2.0", "linux-x86_64", 41, false);
  seed_install(w, "v1.0.0");
  auto const before = read_all(w.inst / "release.json");
  auto const dir    = canon(w.inst) + ".lock";

  struct fault {
    std::string                                     name;
    std::vector<std::string>                        args;
    std::string                                     platform;
    std::optional<std::string>                      ldd;
    std::function<void()>                           arrange;
    std::string                                     message;
    std::map<std::string, std::string, std::less<>> env;
  };
  auto const               sums = w.rel / "download/v1.1.0/SHA256SUMS";
  auto const               good = read_all(sums);
  std::vector<fault> const faults{
      {"checksum mismatch",
       {},
       "macos-arm64",
       std::nullopt,
       [&] {
         auto text = good;
         auto at   = text.find("  planar-macos-arm64.tar.gz");
         text.replace(at - 64, 64, std::string(64, 'c'));
         write_file(sums, text);
       },
       "error: checksum mismatch for planar-macos-arm64.tar.gz: the download does not match SHA256SUMS; nothing was extracted or "
       "installed",
       {}},
      {"unknown tag",
       {"--version", "v9.9.9"},
       "macos-arm64",
       std::nullopt,
       [&] { write_file(sums, good); },
       std::format("error: release v9.9.9 does not exist on the release server {}: ", w.base),
       {}},
      {"malformed VERSION",
       {},
       "macos-arm64",
       std::nullopt,
       [&] { write_file(w.rel / "latest/download/VERSION", "latest-please\n"); },
       std::format("error: {}/latest/download/VERSION holds 'latest-please', which is not a release tag", w.base),
       {}},
      {"unreachable server",
       {"--version", "v1.1.0"},
       "macos-arm64",
       std::nullopt,
       [] {},
       std::format("error: cannot reach the release server at file://{}: ", canon(w.root / "absent")),
       {{"PLANAR_RELEASE_URL", "file://" + canon(w.root / "absent")}}},
      {"old glibc",
       {"--version", "v1.2.0"},
       "linux-x86_64",
       std::string{"ldd (GNU libc) 2.31"},
       [] {},
       "error: this host has glibc 2.31 but this release needs glibc 2.36 or later; nothing was installed",
       {}},
      {"unsupported platform", {"--version", "v1.1.0"}, "", std::nullopt, [] {}, "error: unsupported platform", {}},
  };
  for (auto const& f : faults) {
    INFO("fault: " << f.name);
    f.arrange();
    recorder rec;
    auto     host = host_for(f.platform, f.ldd, rec);
    if (f.platform.empty()) {
      host.platform = [] { return up::platform_for("Linux", "aarch64"); };
    }
    auto const got = invoke(w, f.args, host, f.env);
    INFO(got.out << got.err);
    CHECK(got.code == 1);
    CHECK(got.err.starts_with(f.message));
    CHECK_FALSE(rec.request.has_value());
    CHECK_FALSE(std::filesystem::exists(update_ns(w)));
    CHECK(lock_free(dir));
    CHECK(read_all(w.inst / "release.json") == before);
  }
}

TEST_CASE("update: a symlinked update namespace is refused before anything is created through it", "[update]") {
  auto w = make_world("upnslink");
  publish(w, "v1.1.0", "macos-arm64");
  seed_install(w, "v1.0.0");
  auto const target = w.root / "elsewhere";
  std::filesystem::create_directories(target);
  std::filesystem::create_symlink(target, update_ns(w));
  recorder   rec;
  auto const got = invoke(w, {}, host_for("macos-arm64", std::nullopt, rec));
  INFO(got.out << got.err);
  CHECK(got.code == 1);
  CHECK(got.err.contains("is not a directory of this user"));
  CHECK_FALSE(rec.request.has_value());
  // Nothing was created at the link's target, and ownership was released.
  CHECK(std::filesystem::is_empty(target));
  CHECK(lock_free(canon(w.inst) + ".lock"));
}

TEST_CASE("update: a bundle with an older schema is refused naming both schema versions", "[update]") {
  auto w = make_world("upschema");
  publish(w, "v1.1.0", "macos-arm64", 99);
  publish(w, "v1.0.0", "macos-arm64", 1, false);
  seed_install(w, "v1.1.0");
  auto const before = read_all(w.inst / "release.json");
  recorder   rec;
  auto const got = invoke(w, {"--version", "v1.0.0"}, host_for("macos-arm64", std::nullopt, rec));
  auto const own = planar::db::embedded_max();
  CHECK(got.code == 1);
  CHECK(got.err.contains(std::format("carries database schema version 1, older than schema version {} of this planar", own)));
  CHECK_FALSE(rec.request.has_value());
  CHECK_FALSE(std::filesystem::exists(update_ns(w)));
  CHECK(lock_free(canon(w.inst) + ".lock"));
  CHECK(read_all(w.inst / "release.json") == before);
  CHECK_FALSE(got.db_open);
}

TEST_CASE("update: a failed exec removes the download and releases ownership", "[update]") {
  auto w = make_world("upexecfail");
  publish(w, "v1.1.0", "macos-arm64");
  seed_install(w, "v1.0.0");
  recorder rec;
  rec.result     = std::unexpected(ENOENT);
  auto const got = invoke(w, {}, host_for("macos-arm64", std::nullopt, rec));
  REQUIRE(rec.request.has_value());
  CHECK(got.code == 1);
  CHECK(got.err.contains("cannot run the installer"));
  CHECK(got.err.contains("the download was removed"));
  CHECK_FALSE(std::filesystem::exists(rec.request->argv.back()));
  CHECK_FALSE(std::filesystem::exists(update_ns(w)));
  CHECK(lock_free(canon(w.inst) + ".lock"));
}

TEST_CASE("update: an interrupted install or uninstall is reported before any release verdict", "[update]") {
  auto w = make_world("upjournal");
  publish(w, "v1.0.0", "macos-arm64");
  seed_install(w, "v1.0.0");
  auto const root  = canon(w.inst);
  auto const jf    = w.inst / ".planar-journal";
  auto const retry = std::string{"curl -fsSL https://example.invalid/download/v1.1.0/get-planar.sh | PLANAR_VERSION=v1.1.0 sh"};
  recorder   rec;
  auto const host = host_for("macos-arm64", std::nullopt, rec);

  // Mutating: incomplete, exit 1, with the journal's retry command -- even
  // though release.json and latest agree on v1.0.0.
  write_file(jf, std::format("planar-journal 1\nroot={}\nphase=mutating\ntarget_version=v1.1.0\nretry={}\n", root, retry));
  for (auto const& args : {std::vector<std::string>{"--check"}, std::vector<std::string>{}}) {
    auto const got = invoke(w, args, host);
    CHECK(got.code == 1);
    CHECK(got.out.empty());
    CHECK(
        got.err.contains(std::format("the Planar installation at {} is incomplete: an install of v1.1.0 was interrupted", root)));
    CHECK(got.err.contains(retry));
  }
  CHECK(lock_free(root + ".lock"));
  // Uninstalling: reported as an interrupted uninstall.
  write_file(jf, std::format("planar-journal 1\nroot={}\nphase=uninstalling\n", root));
  auto const un = invoke(w, {"--check"}, host);
  CHECK(un.code == 1);
  CHECK(un.err.contains(std::format("an uninstall of {} was interrupted", root)));
  // A journal for another root is not valid evidence and is never interpreted.
  write_file(jf, "planar-journal 1\nroot=/elsewhere\nphase=mutating\n");
  auto const foreign = invoke(w, {"--check"}, host);
  CHECK(foreign.code == 1);
  CHECK(foreign.err.contains("is not a valid recovery journal"));
  // Prepared and aborted attempts keep the previous completed release.
  for (std::string_view phase : {"prepared", "aborted-before-mutation", "complete"}) {
    write_file(jf, std::format("planar-journal 1\nroot={}\nphase={}\n", root, phase));
    auto const got = invoke(w, {"--check"}, host);
    INFO(phase);
    CHECK(got.code == 0);
    CHECK(got.out == "installed v1.0.0 latest v1.0.0\n");
    auto const plain = invoke(w, {}, host);
    CHECK(plain.code == 0);
    CHECK(plain.out.contains("no changes"));
  }
  CHECK_FALSE(rec.request.has_value());
}

TEST_CASE("update: a live competing owner is refused by name and a dead updater's directory is reclaimed", "[update]") {
  auto w = make_world("upcompete");
  publish(w, "v1.1.0", "macos-arm64");
  seed_install(w, "v1.0.0");
  auto const root = canon(w.inst);
  auto const dir  = root + ".lock";
  recorder   rec;
  auto const host = host_for("macos-arm64", std::nullopt, rec);

  // A shell install holds the lock while it sleeps.
  auto const ready = w.root / "holder.ready";
  std::system(
      std::format(
          "bash -c {} bash {} {} {} > /dev/null 2>&1 &",
          shell_quote(
              R"(source "$1"; planar_lock_acquire "$2" install || exit 1; echo $$ > "$3.tmp"; mv "$3.tmp" "$3"; exec sleep 30)"),
          shell_quote(lock_lib()), shell_quote(root), shell_quote(ready.string()))
          .c_str());
  REQUIRE(planar::cmd::parity::await_sentinel(ready, true, std::chrono::seconds{20}).has_value());
  auto const holder = std::stoi(read_all(ready));
  auto const held   = invoke(w, {}, host);
  CHECK(held.code == 1);
  CHECK(held.err.contains(std::format("another Planar install (pid {}) is changing this installation", holder)));
  CHECK_FALSE(std::filesystem::exists(update_ns(w)));
  ::kill(holder, SIGKILL);
  int status = 0;
  for (int i = 0; i < 200 && ::kill(holder, 0) == 0; ++i) {
    ::waitpid(holder, &status, WNOHANG);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  // An updater that died after recording and creating its directory: the next
  // update proves it dead, removes exactly that directory, and proceeds.
  auto const g     = std::stoull(highest_gen(dir)) + 1;
  auto const ghost = root + "/.planar-update/update-ghost";
  auto const keep  = root + "/.planar-update/unrelated";
  std::filesystem::create_directories(ghost);
  std::filesystem::create_directories(keep);
  write_file(std::format("{}/owner.{}", dir, g),
             record_text(std::to_string(g), "update", k_dead_pid, "ps:gone", lock::node_name(), root, ghost));
  auto const got = invoke(w, {}, host);
  INFO(got.out << got.err);
  CHECK(got.code == 0);
  CHECK(got.out.contains(std::format("reclaimed the mutation lock from an abandoned update pid {}", k_dead_pid)));
  CHECK(got.out.contains("removed the abandoned updater's temporary directory " + ghost));
  CHECK_FALSE(std::filesystem::exists(ghost));
  CHECK(std::filesystem::exists(keep));
  REQUIRE(rec.request.has_value());
  lock::release(lock::ownership{.dir   = dir,
                                .root  = root,
                                .gen   = highest_gen(dir),
                                .nonce = lock::parse_record(std::format("{}/owner.{}", dir, highest_gen(dir)))->nonce});
}

TEST_CASE("update: downloads follow redirects and never send an Authorization header", "[update]") {
  auto w = make_world("upredirect");
  publish(w, "v1.1.0", "macos-arm64");
  seed_install(w, "v1.0.0");
  std::mutex                                           guard;
  std::vector<planar::http::fixture::captured_request> seen;
  auto const                                           rel = w.rel;
  planar::http::fixture::server                        server([&](const planar::http::fixture::captured_request& req) {
    {
      std::scoped_lock const hold(guard);
      seen.push_back(req);
    }
    if (req.target.starts_with("/releases/")) {
      return planar::http::fixture::canned_response{
          .status = 302, .extra_headers = {{"Location", "/object" + req.target.substr(std::string_view{"/releases"}.size())}}};
    }
    auto const path = rel / req.target.substr(std::string_view{"/object/"}.size());
    if (!std::filesystem::is_regular_file(path)) {
      return planar::http::fixture::canned_response{.status = 404, .body = "missing"};
    }
    return planar::http::fixture::canned_response{
        .status = 200, .body = read_all(path), .content_type = "application/octet-stream"};
  });
  recorder                                             rec;
  auto const                                           got =
      invoke(w, {}, host_for("macos-arm64", std::nullopt, rec), {{"PLANAR_RELEASE_URL", server.base_url() + "/releases"}});
  INFO(got.out << got.err);
  CHECK(got.code == 0);
  REQUIRE(rec.request.has_value());
  std::scoped_lock const hold(guard);
  CHECK(seen.size() == 6); // VERSION, SHA256SUMS, asset: each a 302 then the object
  for (auto const& req : seen) {
    CHECK_FALSE(req.header_value("authorization").has_value());
  }
  auto const root = canon(w.inst);
  auto const dir  = root + ".lock";
  lock::release(lock::ownership{.dir   = dir,
                                .root  = root,
                                .gen   = highest_gen(dir),
                                .nonce = lock::parse_record(std::format("{}/owner.{}", dir, highest_gen(dir)))->nonce});
}

TEST_CASE("update: release bases and redirects outside the download policy are refused", "[update]") {
  for (std::string_view bad : {"http://localhost.example/r", "http://127.0.0.1@example.com/r", "https://user@github.com/r",
                               "https://github.com:99999/r", "https://github.com:/r", "file://host/tmp/r", "ftp://github.com/r",
                               "https://github.com/r?x=1", "http://example.com/r", "https://-bad.com/r", "file:///tmp/../etc"}) {
    INFO(bad);
    CHECK_FALSE(up::release_base_valid(up::normalize_base(bad)).has_value());
  }
  for (std::string_view good : {"https://github.com/rdrsss/planar/releases", "http://127.0.0.1:8080/r", "http://localhost/r",
                                "file:///tmp/rel", "https://objects.example.com:443/x"}) {
    INFO(good);
    CHECK(up::release_base_valid(up::normalize_base(good)).has_value());
  }
  // The same grammar as the bootstrap's url_parse, checked against it.
  for (std::string_view probe : {"http://localhost.example/r", "https://github.com/rdrsss/planar/releases", "file://host/tmp/r",
                                 "file:///tmp/rel", "http://127.0.0.1:8080/r", "https://a..b/r"}) {
    std::string out;
    auto const  rc = bash(R"(sed -n '/^url_parse()/,/^}/p' "$1" > "$3.fn"; . "$3.fn"; url_parse "$2" no)",
                          {(source_root() / "scripts/get-planar.sh").string(), std::string{probe},
                           (std::filesystem::temp_directory_path() / std::format("planar-urlparse-{}", ::getpid())).string()},
                          &out);
    INFO(probe << " " << out);
    CHECK((rc == 0) == up::release_base_valid(std::string{probe}).has_value());
  }

  // A redirect to a non-loopback http host or to a file is refused through
  // the verb's download, without contacting it.
  auto w = make_world("upbadredirect");
  seed_install(w, "v1.0.0");
  for (std::string_view target : {"http://localhost.example.invalid/steal", "file:///etc/hosts"}) {
    planar::http::fixture::server server([&](const planar::http::fixture::captured_request&) {
      return planar::http::fixture::canned_response{.status = 302, .extra_headers = {{"Location", std::string{target}}}};
    });
    recorder                      rec;
    auto const                    got = invoke(w, {"--version", "v1.1.0"}, host_for("macos-arm64", std::nullopt, rec),
                                               {{"PLANAR_RELEASE_URL", server.base_url() + "/r"}});
    INFO(target << got.err);
    CHECK(got.code == 1);
    CHECK(got.err.contains(target));
    CHECK(server.request_count() == 1);
    CHECK_FALSE(std::filesystem::exists(update_ns(w)));
  }
}

TEST_CASE("update: exactly one checksum record is selected for the platform", "[update]") {
  auto const h     = std::string(64, 'a');
  auto const asset = std::string{"planar-macos-arm64.tar.gz"};
  CHECK(up::select_checksum_record(std::format("{}  planar-linux-x86_64.tar.gz\n{}  {}\n{}  get-planar.sh\n",
                                               std::string(64, 'b'), h, asset, std::string(64, 'c')),
                                   asset) == h);
  CHECK(up::select_checksum_record(std::format("{}  {}\r\n", h, asset), asset) == h);
  CHECK(up::select_checksum_record("", asset).error() ==
        "SHA256SUMS has no checksum record for planar-macos-arm64.tar.gz; nothing was extracted or installed");
  CHECK(up::select_checksum_record(std::format("{0}  {1}\n{0}  {1}\n", h, asset), asset)
            .error()
            .starts_with("SHA256SUMS has 2 checksum records for"));
  CHECK(
      up::select_checksum_record(std::format("{}  dir/{}\n", h, asset), asset).error().contains("is malformed or names a path"));
  CHECK(up::select_checksum_record(std::format("{} *{}\n", h, asset), asset).error().contains("is malformed or names a path"));
  CHECK(
      up::select_checksum_record(std::format("{}  {}\n", std::string(64, 'G'), asset), asset).error().contains("malformed hash"));
  CHECK(up::select_checksum_record(std::format("{}  {}\n", std::string(63, 'a'), asset), asset).error().contains("malformed"));
}

TEST_CASE("update: the glibc floor and tag grammar match the bootstrap's", "[update]") {
  CHECK_FALSE(up::glibc_refusal("2.36", "ldd (Debian GLIBC 2.36-9+deb12u7) 2.36").has_value());
  CHECK_FALSE(up::glibc_refusal("2.36", "ldd (GNU libc) 2.40").has_value());
  CHECK_FALSE(up::glibc_refusal("2.36", "ldd (GNU libc) 3.1").has_value());
  CHECK(up::glibc_refusal("2.36", "ldd (GNU libc) 2.9") ==
        "this host has glibc 2.9 but this release needs glibc 2.36 or later; nothing was installed");
  CHECK(up::glibc_refusal("2.36", "musl libc (x86_64)")->contains("musl is not supported"));
  CHECK(up::glibc_refusal("2.36", std::nullopt)->starts_with("ldd is not installed"));
  CHECK(up::glibc_refusal("x", "ldd 2.40")->contains("which is not a glibc version"));
  CHECK(up::version_valid("v1.2.3"));
  CHECK(up::version_valid("v10.0.100"));
  for (std::string_view bad : {"1.2.3", "v1.2", "v1.2.3-rc1", "v1..3", "v1.2.3\n", "latest", ""}) {
    CHECK_FALSE(up::version_valid(bad));
  }
  CHECK(up::platform_for("Darwin", "arm64") == std::string{"macos-arm64"});
  CHECK(up::platform_for("Linux", "x86_64") == std::string{"linux-x86_64"});
  CHECK_FALSE(up::platform_for("Darwin", "x86_64").has_value());
  CHECK_FALSE(up::platform_for("Linux", "aarch64").has_value());
}

// ---------------------------------------------------------------------------
// The lock port against the shell library.
// ---------------------------------------------------------------------------

TEST_CASE("update lock: the native start token, canonical path and records agree with the shell", "[update][lock]") {
  std::string out;
  REQUIRE(bash(R"(source "$1"; planar_lock_start_token "$2")", {lock_lib(), std::to_string(::getpid())}, &out) == 0);
  CHECK(lock::start_token(::getpid()).value() + "\n" == out);

  auto       space = make_arena("lockcanon");
  auto const real  = space.cpp_root / "real";
  std::filesystem::create_directories(real / "sub");
  std::filesystem::create_symlink(real, space.cpp_root / "alias");
  std::filesystem::create_symlink("sub/../sub", real / "rel");
  for (auto const& p : {space.cpp_root / "alias" / "rel" / "missing" / ".." / "tail", space.cpp_root / "alias" / "."}) {
    REQUIRE(bash(R"(source "$1/scripts/install-lib/prefix-guard.sh"; planar_canonical_path "$2")",
                 {source_root().string(), p.string()}, &out) == 0);
    CHECK(up::canonical_path(p.string(), "/").value() + "\n" == out);
  }

  // A record the native side writes parses in the shell, field for field.
  auto const root = canon(space.cpp_root / "inst");
  auto const held = lock::acquire(root, "update", root + "/.planar-update/u1");
  REQUIRE(held.has_value());
  REQUIRE(
      bash(
          R"(source "$1"; _pl_read_record "$2" && echo "$_PLR_GEN|$_PLR_OP|$_PLR_PID|$_PLR_START|$_PLR_NODE|$_PLR_NONCE|$_PLR_ROOT|$_PLR_TMP")",
          {lock_lib(), std::format("{}/owner.{}", held->dir, held->gen)}, &out) == 0);
  CHECK(out == std::format("{}|update|{}|{}|{}|{}|{}|{}\n", held->gen, ::getpid(), lock::start_token(::getpid()).value(),
                           lock::node_name(), held->nonce, root, root + "/.planar-update/u1"));
  lock::release(*held);
  CHECK(std::filesystem::exists(std::format("{}/released.{}", held->dir, held->gen)));
  // Released: the shell takes the next generation and the native side sees it held.
  auto const shell_root = root;
  auto const ready      = space.cpp_root / "ready";
  std::system(
      std::format(
          "bash -c {} bash {} {} {} > /dev/null 2>&1 &",
          shell_quote(
              R"(source "$1"; planar_lock_acquire "$2" uninstall || exit 1; echo $$ > "$3.tmp"; mv "$3.tmp" "$3"; exec sleep 30)"),
          shell_quote(lock_lib()), shell_quote(shell_root), shell_quote(ready.string()))
          .c_str());
  REQUIRE(planar::cmd::parity::await_sentinel(ready, true, std::chrono::seconds{20}).has_value());
  auto const pid     = std::stoi(read_all(ready));
  auto const refused = lock::acquire(root, "update", "");
  REQUIRE_FALSE(refused.has_value());
  CHECK(refused.error().starts_with(std::format("another Planar uninstall (pid {}) is changing this installation", pid)));
  ::kill(pid, SIGKILL);
  for (int i = 0; i < 200 && ::kill(pid, 0) == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Dead: reclaimed, the reclaimed owner named, and the lower generations housekept.
  auto const again = lock::acquire(root, "update", "");
  REQUIRE(again.has_value());
  CHECK(again->reclaimed == std::format("uninstall pid {}", pid));
  CHECK_FALSE(std::filesystem::exists(std::format("{}/owner.{}", again->dir, held->gen)));
  lock::release(*again);
}

TEST_CASE("update lock: a reused pid is reclaimed and ambiguous records refuse on both sides", "[update][lock]") {
  auto space = make_arena("lockambig");
  struct shape {
    std::string name;
    std::string body; // owner.1, empty for a symlink
    bool        free; // native and shell both reclaim
  };
  auto const               me = std::to_string(::getpid());
  std::vector<shape> const shapes{
      {"reused pid", record_text("1", "install", me, "ps:Thu Jan 1 00:00:00 1970", lock::node_name(), "/r"), true},
      {"dead pid", record_text("1", "install", k_dead_pid, "ps:x", lock::node_name(), "/r"), true},
      {"another host", record_text("1", "install", me, "ps:x", "elsewhere.invalid", "/r"), false},
      {"malformed", "planar-mutation-lock 1\ngen=1\noperation=install\n", false},
      {"wrong generation", record_text("7", "install", k_dead_pid, "ps:x", lock::node_name(), "/r"), false},
      {"unknown key", record_text("1", "install", k_dead_pid, "ps:x", lock::node_name(), "/r") + "extra=1\n", false},
      {"symlinked record", "", false},
  };
  int n = 0;
  for (auto const& s : shapes) {
    INFO(s.name);
    for (std::string_view side : {"native", "shell"}) {
      auto const root = canon(space.cpp_root / std::format("r{}", n++));
      auto const dir  = root + ".lock";
      std::filesystem::create_directories(dir);
      std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
      if (s.body.empty()) {
        write_file(dir + "/target", record_text("1", "install", k_dead_pid, "ps:x", lock::node_name(), root));
        std::filesystem::create_symlink(dir + "/target", dir + "/owner.1");
      } else {
        write_file(dir + "/owner.1", s.body);
      }
      bool won = false;
      if (side == "native") {
        auto const got = lock::acquire(root, "update", "");
        won            = got.has_value();
        if (!won) {
          CHECK(got.error().contains("cannot be judged free"));
        }
      } else {
        std::string out;
        won = bash(R"(source "$1"; planar_lock_acquire "$2" install)", {lock_lib(), root}, &out) == 0;
      }
      INFO(side);
      CHECK(won == s.free);
      if (!s.free) {
        CHECK(std::filesystem::exists(std::filesystem::symlink_status(dir + "/owner.1")));
        CHECK_FALSE(std::filesystem::exists(dir + "/owner.2"));
      }
    }
  }
}

TEST_CASE("update lock: simultaneous reclaims of a dead owner leave exactly one owner", "[update][lock]") {
  auto       space = make_arena("lockrace");
  auto const root  = canon(space.cpp_root / "inst");
  auto const dir   = root + ".lock";
  std::filesystem::create_directories(dir);
  std::filesystem::permissions(dir, std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
  write_file(dir + "/owner.1", record_text("1", "install", k_dead_pid, "ps:x", lock::node_name(), root));
  auto const    go       = space.cpp_root / "go";
  constexpr int k_racers = 6;
  for (int i = 0; i < k_racers; ++i) {
    std::system(std::format("bash -c {} bash {} {} {} {} > /dev/null 2>&1 &",
                            shell_quote(R"(source "$1"; while [ ! -e "$3" ]; do :; done
if planar_lock_acquire "$2" install; then echo won > "$4.tmp"; else echo lost > "$4.tmp"; fi; mv "$4.tmp" "$4"; exec sleep 5)"),
                            shell_quote(lock_lib()), shell_quote(root), shell_quote(go.string()),
                            shell_quote((space.cpp_root / std::format("racer{}", i)).string()))
                    .c_str());
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  write_file(go, "");
  auto const native = lock::acquire(root, "update", "");
  int        wins   = native.has_value() ? 1 : 0;
  for (int i = 0; i < k_racers; ++i) {
    auto const result = space.cpp_root / std::format("racer{}", i);
    REQUIRE(planar::cmd::parity::await_sentinel(result, true, std::chrono::seconds{30}).has_value());
    wins += read_all(result).starts_with("won") ? 1 : 0;
  }
  CHECK(wins == 1);
  // Exactly one live, unreleased owner at the top; every lower generation is gone.
  CHECK(highest_gen(dir) == "2");
  CHECK_FALSE(std::filesystem::exists(dir + "/released.2"));
  if (native.has_value()) {
    lock::release(*native);
  }
}

TEST_CASE("update lock: an unsafe coordination directory is refused with the shell's diagnostic", "[update][lock]") {
  auto       space = make_arena("lockdir");
  auto const root  = canon(space.cpp_root / "inst");
  auto const dir   = root + ".lock";
  std::filesystem::create_directories(dir);
  std::filesystem::permissions(dir, std::filesystem::perms::owner_all | std::filesystem::perms::group_write,
                               std::filesystem::perm_options::replace);
  auto const got = lock::acquire(root, "update", "");
  REQUIRE_FALSE(got.has_value());
  std::string out;
  CHECK(bash(R"(source "$1"; planar_lock_acquire "$2" install || echo "$PLANAR_LOCK_ERROR")", {lock_lib(), root}, &out) == 0);
  // The same refusal, word for word, up to the mode: the shell prints `ls -ld`'s
  // field, which can carry a trailing extended-attribute or ACL mark.
  auto const until_mode = [](std::string_view text) { return std::string{text.substr(0, text.find(" (mode "))}; };
  CHECK(until_mode(got.error()) == until_mode(out));
  CHECK(got.error().contains("is writable by its group or by others (mode drwx-w----"));
  CHECK(got.error().ends_with(out.substr(out.find(')')).substr(0, out.size() - out.find(')') - 1)));

  auto const other = canon(space.cpp_root / "other");
  std::filesystem::create_directories(space.cpp_root / "target");
  std::filesystem::create_symlink(space.cpp_root / "target", other + ".lock");
  auto const linked = lock::acquire(other, "update", "");
  REQUIRE_FALSE(linked.has_value());
  CHECK(linked.error().starts_with(std::format("the mutation lock directory {}.lock is a symlink", other)));

  // update_tmp_valid agrees with planar_update_tmp_valid.
  auto const r = canon(space.cpp_root / "tmpcheck");
  std::filesystem::create_directories(r + "/.planar-update/ok-1");
  std::filesystem::create_directories(space.cpp_root / "elsewhere");
  std::filesystem::create_symlink(space.cpp_root / "elsewhere", r + "/.planar-update/link");
  for (auto const& d : {r + "/.planar-update/ok-1", r + "/.planar-update/link", r + "/.planar-update/.hidden",
                        r + "/.planar-update", r + "/.planar-update/ok-1/..", r + "/workbench", r + "/.planar-update/missing"}) {
    INFO(d);
    auto const shell = bash(R"(source "$1"; planar_update_tmp_valid "$2" "$3")", {lock_lib(), r, d}) == 0;
    CHECK(lock::update_tmp_valid(r, d) == shell);
  }
  CHECK(lock::update_tmp_valid(r, r + "/.planar-update/ok-1"));
}

// ---------------------------------------------------------------------------
// The built binary, black-box.
// ---------------------------------------------------------------------------

namespace {

auto cpp_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

/// This host's bundle platform, or unset when no bundle exists for it.
auto host_platform() -> std::optional<std::string> {
  auto const got = planar::cmd::handlers::native_host().platform();
  return got.has_value() ? std::optional{*got} : std::nullopt;
}

/// The pinned arena environment plus the release base (and any extras).
auto env_with(const std::filesystem::path& work, const std::string& base, std::vector<pinned_var> extra = {})
    -> std::vector<pinned_var> {
  auto env = pinned_env(work);
  env.push_back({.name = "PLANAR_RELEASE_URL", .value = base});
  for (auto& v : extra) {
    env.push_back(std::move(v));
  }
  return env;
}

} // namespace

TEST_CASE("update: the binary updates a bootstrap install end to end through the real installer", "[update][e2e]") {
  auto       space = make_arena("upe2e");
  auto const work  = space.cpp_root;
  auto const rel   = work / "rel";
  auto const base  = "file://" + canon(rel);
  auto const plat  = host_platform();
  if (!plat.has_value()) {
    auto const got = run_pinned(cpp_bin(), std::vector<std::string>{"update"}, work, "unsupported", env_with(work, base));
    CHECK(got.code == 1);
    CHECK(got.err.starts_with("error: unsupported platform"));
    return;
  }
  for (auto const& [tag, schema] : {std::pair{"v1.0.0", 41}, std::pair{"v1.1.0", 99}}) {
    std::string out;
    REQUIRE(bash(R"("$1/src/cmd/planar/handlers/update/release_fixture.sh" "$@")",
                 {source_root().string(), rel.string(), tag, *plat, std::to_string(schema)}, &out) == 0);
  }
  // First install with the real bootstrap, pinned to v1.0.0.
  auto const inst = work / "home"; // the pinned PLANAR_HOME
  std::filesystem::remove_all(inst);
  std::string boot;
  auto const  boot_rc =
      bash(std::format("cd {} && env -u PLANAR_QUEUE_SLOT HOME={} PLANAR_HOME={} PLANAR_DB={} PLANAR_CONFIG_PATH={} "
                       "PLANAR_VERSION=v1.0.0 PLANAR_RELEASE_URL={} sh {}",
                       shell_quote(work.string()), shell_quote((work / "fakehome").string()), shell_quote(inst.string()),
                       shell_quote((work / "planar.db").string()), shell_quote((work / "config.toml").string()),
                       shell_quote(base), shell_quote((source_root() / "scripts/get-planar.sh").string())),
           {}, &boot);
  INFO(boot);
  REQUIRE(boot_rc == 0);
  REQUIRE(read_all(inst / "release.json").contains("\"v1.0.0\""));

  auto const check = run_pinned(cpp_bin(), std::vector<std::string>{"update", "--check"}, work, "check", env_with(work, base));
  CHECK(check.code == 10);
  CHECK(check.out == "installed v1.0.0 latest v1.1.0\n");

  auto const got = run_pinned(cpp_bin(), std::vector<std::string>{"update"}, work, "update", env_with(work, base));
  INFO(got.out << got.err);
  REQUIRE(got.code == 0);
  CHECK((got.out + got.err).contains("adopted the updater's ownership"));
  CHECK(read_all(inst / "release.json").contains("\"version\": \"v1.1.0\""));
  // Every managed subtree matches the new bundle.
  auto const unpacked = work / "unpacked";
  std::filesystem::create_directories(unpacked);
  REQUIRE(bash(R"(tar -xzf "$1" -C "$2")",
               {(rel / std::format("download/v1.1.0/planar-{}.tar.gz", *plat)).string(), unpacked.string()}) == 0);
  for (std::string_view sub : {"bin", "skills", "agents", "codex-agents", "workflows", "migrations"}) {
    INFO(sub);
    // Every bundle file is installed byte for byte (the installer adds
    // bin/planar-uninstall beside them).
    CHECK(bash(R"(cd "$1" && find . -type f | while IFS= read -r f; do cmp -s "$f" "$2/$f" || { echo "$f"; exit 1; }; done)",
               {(unpacked / std::format("planar-{}", *plat) / sub).string(), (inst / sub).string()}) == 0);
  }
  // The temporary directory is gone and ownership was consumed and released.
  CHECK_FALSE(std::filesystem::exists(inst / ".planar-update"));
  auto const dir = canon(inst) + ".lock";
  auto const g   = highest_gen(dir);
  CHECK(std::filesystem::exists(std::format("{}/handoff.{}", dir, g)));
  CHECK(std::filesystem::exists(std::format("{}/released.{}", dir, g)));
  CHECK(lock::parse_record(std::format("{}/owner.{}", dir, g))->operation == "update");

  auto const again = run_pinned(cpp_bin(), std::vector<std::string>{"update"}, work, "again", env_with(work, base));
  CHECK(again.code == 0);
  CHECK(again.out.contains("no changes"));
}

TEST_CASE("update: the exec'd installer is the lock owner and the verb opens no database", "[update][e2e]") {
  auto       space = make_arena("upexec");
  auto const work  = space.cpp_root;
  auto const rel   = work / "rel";
  auto const base  = "file://" + canon(rel);
  auto const plat  = host_platform();
  if (!plat.has_value()) {
    SUCCEED("no release bundle exists for this host; the in-process hand-off case covers the request");
    return;
  }
  std::string out;
  REQUIRE(bash(R"("$1/src/cmd/planar/handlers/update/release_fixture.sh" "$@")",
               {source_root().string(), rel.string(), "v1.1.0", *plat}, &out) == 0);
  auto const inst = work / "home";
  write_file(inst / "release.json", "{\"version\": \"v1.0.0\", \"schema_version\": 41}\n");
  write_file(inst / ".planar-install", "x\ny\n");
  // A `bash` that reports what the exec'd process sees, from inside it.
  auto const fake   = work / "fakebin";
  auto const report = work / "exec.report";
  write_file(fake / "bash", std::format(R"SH(#!/bin/sh
r={}
g="${{PLANAR_MUTATION_HANDOFF%%:*}}"
l="$(dirname "$(dirname "$5")")"
{{
  echo "pid=$$"
  echo "args=$*"
  echo "handoff=$PLANAR_MUTATION_HANDOFF"
  echo "expect=${{PLANAR_EXPECT_RECOVERY-unset}}"
  echo "released=$(test -e "$l.lock/released.$g" && echo yes || echo no)"
  grep -E '^(pid|nonce|operation|tmp)=' "$l.lock/owner.$g"
}} > "$r"
exit 0
)SH",
                                        shell_quote(report.string())));
  std::filesystem::permissions(fake / "bash", std::filesystem::perms::owner_all);
  auto const path = std::format("{}:{}", fake.string(), std::getenv("PATH"));
  auto const got =
      run_pinned(cpp_bin(), std::vector<std::string>{"update"}, work, "exec",
                 env_with(work, base, {{.name = "PATH", .value = path}, {.name = "PLANAR_EXPECT_RECOVERY", .value = "x"}}));
  INFO(got.out << got.err);
  REQUIRE(got.code == 0);
  auto const text = read_all(report);
  INFO(text);
  auto const field = [&](std::string_view key) {
    auto const at   = text.find(std::format("\n{}=", key));
    auto const from = at == std::string::npos ? (text.starts_with(std::format("{}=", key)) ? key.size() + 1 : std::string::npos)
                                              : at + key.size() + 2;
    return from == std::string::npos ? std::string{} : text.substr(from, text.find('\n', from) - from);
  };
  auto const first_pid = text.substr(4, text.find('\n') - 4);
  // The record's pid is the exec'd process's own: exec kept it.
  CHECK(text.contains(std::format("\npid={}\n", first_pid)));
  CHECK(field("released") == "no");
  CHECK(field("operation") == "update");
  CHECK(field("handoff").ends_with(":" + field("nonce")));
  CHECK(field("expect") == "unset");
  auto const tmp    = field("tmp");
  auto const bundle = std::format("{}/x/planar-{}", tmp, *plat);
  CHECK(field("args") == std::format("{}/install.sh --prebuilt {} --cleanup {}", bundle, bundle, tmp));
  CHECK(tmp.starts_with(canon(inst) + "/.planar-update/update-"));
  // The verb never opened (or created) the database.
  CHECK_FALSE(std::filesystem::exists(work / "planar.db"));
}

TEST_CASE("update: a KILLed update leaves nothing the next update cannot reclaim", "[update][e2e]") {
  auto       space = make_arena("upkill");
  auto const work  = space.cpp_root;
  auto const rel   = work / "rel";
  auto const plat  = host_platform();
  if (!plat.has_value()) {
    SUCCEED("no release bundle exists for this host; the in-process reclaim case covers the recovery");
    return;
  }
  std::string out;
  REQUIRE(bash(R"("$1/src/cmd/planar/handlers/update/release_fixture.sh" "$@")",
               {source_root().string(), rel.string(), "v1.1.0", *plat}, &out) == 0);
  auto const inst = work / "home";
  write_file(inst / "release.json", "{\"version\": \"v1.0.0\", \"schema_version\": 41}\n");
  write_file(inst / ".planar-install", "x\ny\n");
  write_file(inst / "bin/planar", "#!/bin/sh\n");
  // A server that answers everything but stalls the asset long enough to KILL the download.
  planar::http::fixture::server server([&](const planar::http::fixture::captured_request& req) {
    auto const path = rel / req.target.substr(1);
    auto       body = read_all(path);
    if (req.target.ends_with(".tar.gz")) {
      return planar::http::fixture::canned_response{.status                  = 200,
                                                    .body                    = body,
                                                    .content_type            = "application/octet-stream",
                                                    .chunk_bytes             = 16,
                                                    .stall_after_first_chunk = true,
                                                    .stall_delay             = std::chrono::seconds{15}};
    }
    return planar::http::fixture::canned_response{.status = 200, .body = body, .content_type = "text/plain"};
  });
  planar::cmd::parity::launch_pinned_detached(cpp_bin(), std::vector<std::string>{"update"}, work, "killed",
                                              env_with(work, server.base_url()));
  auto const dir = canon(inst) + ".lock";
  REQUIRE(planar::cmd::parity::await_sentinel(dir + "/owner.1", true, std::chrono::seconds{30}).has_value());
  auto const rec = lock::parse_record(dir + "/owner.1");
  REQUIRE(rec.has_value());
  REQUIRE(planar::cmd::parity::await_sentinel(rec->tmp + "/.", false, std::chrono::seconds{30}).has_value());
  // Wait until the asset request is in flight, then KILL: no trap can run.
  for (int i = 0; i < 3000 && server.request_count() < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ::kill(std::stoi(rec->pid), SIGKILL);
  for (int i = 0; i < 500 && ::kill(std::stoi(rec->pid), 0) == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  CHECK(std::filesystem::exists(rec->tmp));
  CHECK_FALSE(std::filesystem::exists(dir + "/released.1"));

  auto const next =
      run_pinned(cpp_bin(), std::vector<std::string>{"update"}, work, "next", env_with(work, "file://" + canon(rel)));
  INFO(next.out << next.err);
  CHECK(next.code == 0);
  CHECK(next.out.contains(std::format("reclaimed the mutation lock from an abandoned update pid {}", rec->pid)));
  CHECK_FALSE(std::filesystem::exists(rec->tmp));
  CHECK_FALSE(std::filesystem::exists(inst / ".planar-update"));
  CHECK(read_all(inst / "release.json").contains("\"version\": \"v1.1.0\""));
  CHECK(lock_free(dir));
}

TEST_CASE("update: --check runs from inside a linked git worktree and is not refused as a planning verb", "[update][e2e]") {
  // `update` writes no planning state, so the worktree gate must let it
  // through. The bypass is removed from the child's environment, and a
  // planning verb from the same directory is the control that proves the
  // directory really is a worktree the gate refuses from.
  if (std::system("git --version >/dev/null 2>&1") != 0) {
    SKIP("git not on PATH");
  }
  auto        space = make_arena("upwtree");
  auto const  work  = space.cpp_root;
  auto const  rel   = work / "rel";
  auto const  base  = "file://" + canon(rel);
  std::string out;
  REQUIRE(bash(R"("$1/src/cmd/planar/handlers/update/release_fixture.sh" "$@")",
               {source_root().string(), rel.string(), "v1.1.0", "macos-arm64"}, &out) == 0);
  write_file(work / "home/release.json", "{\"version\": \"v1.0.0\", \"schema_version\": 41}\n");
  auto const repo = work / "mainrepo";
  std::filesystem::create_directories(repo);
  std::filesystem::remove_all(work / "proj");
  REQUIRE(bash(R"(cd "$1" && git init -q -b main && git -c user.email=planar@example.invalid -c user.name=Planar )"
               R"(commit -q --allow-empty -m seed && git worktree add -q -b side "$2")",
               {repo.string(), (work / "proj").string()}) == 0);

  auto const env     = env_with(work, base, {pinned_var{.name = "PLANAR_DISABLE_WORKTREE_GATE", .value = "", .unset = true}});
  auto const control = run_pinned(cpp_bin(), std::vector<std::string>{"plan", "create", "refused"}, work, "control", env);
  REQUIRE(control.code == 8);

  auto const check = run_pinned(cpp_bin(), std::vector<std::string>{"update", "--check"}, work, "check", env);
  INFO(check.out << check.err);
  CHECK(check.code == 10);
  CHECK(check.out == "installed v1.0.0 latest v1.1.0\n");
  CHECK_FALSE(check.err.contains("may not run from inside a worktree"));
}
