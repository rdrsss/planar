// queue_run_claim.t.cpp: `planar-agent queue run --claim <token>` as an operator
// sees it (plan 1080, tasks hq-claim-renewal and hq-claim-renewal-nonfatal;
// tech spec 647 § CLI surface, § Whether the submitter should renew the
// agent's claim).
//
// The rule under test: `--claim` makes the submitter renew that claim at half
// its lease interval while the entry waits and while the command runs, and a
// renewal that fails is reported once on standard error and never stops the
// command. Claims live in the MAIN database, which `queue run` otherwise never
// opens, so the arena here has BOTH a scratch `PLANAR_DB` (seeded through the
// engine migrations, holding a real task and a claim the built `claim` verb
// minted) and a scratch `PLANAR_AGENT_DB`. Every invocation runs the built
// binary through the harness's pinned environment, so nothing can reach the
// operator's `~/.planar`.
//
// Renewal is observed as the claim row's `lease_expires_at` moving in the arena
// main database. Synchronisation is by FIFOs and sentinel files, exactly as in
// queue_run.t.cpp; the only clocks are bounded waits, and the lease is short
// (4 seconds, so a renewal every 2) so that a renewal is observable in a
// bounded case.

#include <catch2/catch_test_macros.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

import std;
import planar.db;
import planar.db.agentdb;
import planar.db.migrate;
import planar.process.identity;
import planar.engine.hostqueue;

#include "parity_harness.hpp"

namespace {

namespace hq     = planar::engine::hostqueue;
namespace ident  = planar::process::identity;
namespace parity = planar::cmd::parity;

using parity::capture;
using parity::pinned_var;
using parity::read_all;

constexpr auto k_budget = std::chrono::seconds(30);

auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

auto main_db(const parity::arena& arena) -> std::filesystem::path {
  return arena.cpp_root / "planar.db";
}

auto queue_args(const std::vector<std::string>& command, std::vector<std::string> flags = {}) -> std::vector<std::string> {
  std::vector<std::string> args{"queue", "run"};
  for (auto& flag : flags) {
    args.push_back(std::move(flag));
  }
  args.emplace_back("--");
  for (auto const& word : command) {
    args.push_back(word);
  }
  return args;
}

auto sh_command(std::string script, std::vector<std::string> args = {}) -> std::vector<std::string> {
  std::vector<std::string> command{"sh", "-c", std::move(script), "sh"};
  for (auto& arg : args) {
    command.push_back(std::move(arg));
  }
  return command;
}

auto run_queue(const parity::arena& arena, std::string_view tag, const std::vector<std::string>& command,
               std::vector<std::string> flags = {}) -> capture {
  auto const args = queue_args(command, std::move(flags));
  return parity::run_pinned(agent_bin(), args, arena.cpp_root, tag);
}

void write_config(const parity::arena& arena, std::string_view text) {
  auto const path = arena.cpp_root / "config.toml";
  auto const temp = arena.cpp_root / "config.toml.new";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out << text;
  }
  std::filesystem::rename(temp, path);
}

constexpr std::string_view k_fast_poll = "[queue]\npoll_interval = \"100ms\"\n";

template <class Predicate> auto await(Predicate&& predicate, std::chrono::milliseconds budget = k_budget) -> bool {
  auto const deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() <= deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return predicate();
}

auto present(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

void await_file(const std::filesystem::path& path) {
  INFO("waiting for " << path.string());
  REQUIRE(await([&] { return present(path); }));
}

// --- the agent database, read back through the engine --------------------

struct snapshot {
  std::vector<hq::entry>       entries;
  std::vector<hq::history_row> history;
};

auto try_snapshot(const parity::arena& arena) -> std::optional<snapshot> {
  auto const path = arena.cpp_root / "agent.db";
  if (!present(path)) {
    return std::nullopt;
  }
  auto opened = planar::db::agent::open_agent_db_at(path);
  if (!opened) {
    return std::nullopt;
  }
  auto entries = hq::list(*opened);
  auto history = hq::list_history(*opened);
  if (!entries || !history) {
    return std::nullopt;
  }
  return snapshot{.entries = std::move(*entries), .history = std::move(*history)};
}

auto require_snapshot(const parity::arena& arena) -> snapshot {
  auto const now = try_snapshot(arena);
  REQUIRE(now.has_value());
  return *now;
}

auto entry_seq(const snapshot& snap, std::int64_t seq) -> const hq::entry* {
  auto const found = std::ranges::find_if(snap.entries, [&](const hq::entry& e) { return e.seq == seq; });
  return found == snap.entries.end() ? nullptr : &*found;
}

auto history_seq(const snapshot& snap, std::int64_t seq) -> const hq::history_row* {
  auto const found = std::ranges::find_if(snap.history, [&](const hq::history_row& r) { return r.seq == seq; });
  return found == snap.history.end() ? nullptr : &*found;
}

auto await_entry(const parity::arena& arena, std::int64_t seq, hq::entry_state state) -> hq::entry {
  std::optional<hq::entry> seen;
  REQUIRE(await([&] {
    auto const snap = try_snapshot(arena);
    if (!snap) {
      return false;
    }
    auto const* found = entry_seq(*snap, seq);
    if (found != nullptr && found->state == state) {
      seen = *found;
      return true;
    }
    return false;
  }));
  return *seen;
}

auto await_history(const parity::arena& arena, std::int64_t seq) -> hq::history_row {
  std::optional<hq::history_row> seen;
  REQUIRE(await([&] {
    auto const snap = try_snapshot(arena);
    if (!snap) {
      return false;
    }
    auto const* found = history_seq(*snap, seq);
    if (found != nullptr) {
      seen = *found;
      return true;
    }
    return false;
  }));
  return *seen;
}

// --- the main database: a real task and a real claim -----------------------

/// @brief Migrates the arena's main database and seeds one plan and one task.
void seed_main(const parity::arena& arena) {
  auto conn = planar::db::connection::open(main_db(arena).string());
  REQUIRE(conn.has_value());
  REQUIRE(planar::db::apply_all(*conn).has_value());
  REQUIRE(conn->execute("insert into plans (scope_kind, title, slug, status) values ('global','p','claim-plan','active')")
              .has_value());
  REQUIRE(conn->execute("insert into tasks (scope_kind, plan_id, title, status, priority) values ('global', 1, 't', 'todo', 100)")
              .has_value());
}

/// @brief Mints a claim on the seeded task through the built `claim` verb and
/// returns its token. `ttl` is the lease, so half of it is the cadence.
auto mint_claim(const parity::arena& arena, std::string_view ttl = "4s") -> std::string {
  std::vector<std::string> const args{"claim", "--entity", "task:1", "--ttl", std::string{ttl}};
  auto const                     got = parity::run_pinned(agent_bin(), args, arena.cpp_root, "claim");
  INFO("claim stdout:\n" << got.out << "\nstderr:\n" << got.err);
  REQUIRE(got.code == 0);
  constexpr std::string_view prefix = "claim:";
  REQUIRE(got.out.starts_with(prefix));
  auto const end = got.out.find_first_of(" \n", prefix.size());
  return got.out.substr(prefix.size(), end - prefix.size());
}

/// @brief Reads one text column of the claim row, by token.
auto claim_field(const parity::arena& arena, const std::string& token, std::string_view column) -> std::string {
  auto conn = planar::db::connection::open(main_db(arena).string());
  REQUIRE(conn.has_value());
  auto stmt = conn->prepare(std::format("select {} from agent_work_claims where claim_token = ?", column));
  REQUIRE(stmt.has_value());
  REQUIRE(stmt->bind_text(1, token).has_value());
  auto step = stmt->step();
  REQUIRE(step.has_value());
  REQUIRE(*step == planar::db::step_result::row);
  return stmt->column_text(0);
}

auto expiry(const parity::arena& arena, const std::string& token) -> std::string {
  return claim_field(arena, token, "lease_expires_at");
}

/// @brief Waits until the claim's lease expiry differs from `previous` and
/// returns the new one.
auto await_renewal(const parity::arena& arena, const std::string& token, const std::string& previous) -> std::string {
  std::string seen;
  INFO("waiting for the claim's lease expiry to move from " << previous);
  REQUIRE(await([&] {
    seen = expiry(arena, token);
    return seen != previous;
  }));
  return seen;
}

/// @brief How many stderr lines begin with `warning: queue:`.
auto warning_count(const std::string& text) -> std::size_t {
  std::size_t count = 0;
  std::size_t at    = 0;
  while (at < text.size()) {
    auto const end = text.find('\n', at);
    auto const len = (end == std::string::npos ? text.size() : end) - at;
    if (std::string_view{text}.substr(at, len).starts_with("warning: queue:")) {
      ++count;
    }
    if (end == std::string::npos) {
      break;
    }
    at = end + 1;
  }
  return count;
}

auto last_line(std::string_view text) -> std::string {
  while (text.ends_with('\n')) {
    text.remove_suffix(1);
  }
  auto const start = text.rfind('\n');
  return std::string{start == std::string_view::npos ? text : text.substr(start + 1)};
}

// --- gates and background submitters ---------------------------------------

struct gate {
  std::filesystem::path path;
  int                   fd       = -1;
  bool                  released = false;

  explicit gate(std::filesystem::path where) : path(std::move(where)) {
    REQUIRE(::mkfifo(path.c_str(), 0600) == 0);
    fd = ::open(path.c_str(), O_RDWR);
    REQUIRE(fd >= 0);
  }
  gate(const gate&)            = delete;
  gate& operator=(const gate&) = delete;
  ~gate() {
    release();
    if (fd >= 0) {
      ::close(fd);
    }
  }
  void release() {
    if (!released && fd >= 0) {
      released = true;
      static_cast<void>(::write(fd, "x\n", 2));
    }
  }
};

struct release_all {
  std::vector<gate*> gates;
  ~release_all() {
    for (auto* g : gates) {
      g->release();
    }
  }
};

/// @brief A `queue run` started in the background. It owns the submitter it
/// started and stops it (SIGTERM to that one recorded pid, then SIGKILL) if it
/// is still alive when the case ends. Nothing is signalled by name.
struct spawned {
  std::filesystem::path            root;
  std::string                      tag;
  std::int64_t                     pid = 0;
  std::optional<ident::start_time> started;

  spawned() = default;
  spawned(std::filesystem::path where, std::string name, std::int64_t submitter, std::optional<ident::start_time> when)
      : root(std::move(where)), tag(std::move(name)), pid(submitter), started(when) {
  }
  spawned(const spawned&)            = delete;
  spawned& operator=(const spawned&) = delete;
  spawned(spawned&& other) noexcept
      : root(std::move(other.root)), tag(std::move(other.tag)), pid(std::exchange(other.pid, 0)), started(other.started) {
  }
  spawned& operator=(spawned&& other) noexcept {
    if (this != &other) {
      stop();
      root    = std::move(other.root);
      tag     = std::move(other.tag);
      pid     = std::exchange(other.pid, 0);
      started = other.started;
    }
    return *this;
  }
  ~spawned() {
    stop();
  }

  [[nodiscard]] auto ended() const -> bool {
    return read_all(root / std::format("{}.code", tag)).ends_with('\n');
  }
  [[nodiscard]] auto still_mine() const -> bool {
    if (pid <= 1 || !started) {
      return false;
    }
    auto const now = ident::process_start_time(pid);
    return now && now->has_value() && **now == *started;
  }
  void stop() {
    if (pid <= 1 || ended()) {
      pid = 0;
      return;
    }
    if (still_mine()) {
      ::kill(static_cast<::pid_t>(pid), SIGTERM);
    }
    if (!await([&] { return ended(); }, std::chrono::seconds(10)) && still_mine()) {
      ::kill(static_cast<::pid_t>(pid), SIGKILL);
      await([&] { return ended(); }, std::chrono::seconds(10));
    }
    pid = 0;
  }
};

auto spawn_queue(const parity::arena& arena, std::string tag, const std::vector<std::string>& command,
                 std::vector<std::string> flags = {}) -> spawned {
  auto const vars = parity::pinned_env(arena.cpp_root);
  parity::require_agent_db_pinned(arena.cpp_root, vars);

  std::string child = parity::pinned_env_prefix(vars) + parity::shell_quote(agent_bin().string());
  for (auto const& arg : queue_args(command, std::move(flags))) {
    child += " " + parity::shell_quote(arg);
  }
  auto const path = [&](std::string_view suffix) {
    return parity::shell_quote((arena.cpp_root / std::format("{}.{}", tag, suffix)).string());
  };
  std::error_code ec;
  std::filesystem::remove(arena.cpp_root / std::format("{}.code", tag), ec);
  std::filesystem::remove(arena.cpp_root / std::format("{}.pid", tag), ec);
  auto const line = std::format(
      "( cd {} && {{ {} & echo $! > {} ; wait $! ; echo $? > {} ; }} > {} 2> {} ) </dev/null >/dev/null "
      "2>&1 &",
      parity::shell_quote((arena.cpp_root / "proj").string()), child, path("pid"), path("code"), path("out"), path("err"));
  static_cast<void>(std::system(line.c_str()));

  std::int64_t                     pid = 0;
  std::optional<ident::start_time> started;
  await([&] {
    auto const text = read_all(arena.cpp_root / std::format("{}.pid", tag));
    if (!text.ends_with('\n')) {
      return false;
    }
    std::int64_t value = 0;
    if (std::from_chars(text.data(), text.data() + text.size() - 1, value).ec != std::errc{} || value <= 1) {
      return false;
    }
    pid = value;
    return true;
  });
  if (pid > 1) {
    if (auto const at = ident::process_start_time(pid); at && at->has_value()) {
      started = **at;
    }
  }
  return spawned{arena.cpp_root, std::move(tag), pid, started};
}

auto finish(const spawned& run) -> capture {
  auto const code_path = run.root / std::format("{}.code", run.tag);
  INFO("waiting for " << run.tag << " to end");
  REQUIRE(await([&] { return read_all(code_path).ends_with('\n'); }));
  auto const  raw  = read_all(code_path);
  int         code = -1;
  auto const* head = raw.data();
  REQUIRE(std::from_chars(head, head + raw.size() - 1, code).ec == std::errc{});
  return capture{.code = code,
                 .out  = read_all(run.root / std::format("{}.out", run.tag)),
                 .err  = read_all(run.root / std::format("{}.err", run.tag))};
}

/// @brief `echo x > $1; read x < $2` — announces it started, then blocks.
auto blocked(const std::filesystem::path& started, const gate& hold) -> std::vector<std::string> {
  return sh_command("echo x > \"$1\"; read x < \"$2\"", {started.string(), hold.path.string()});
}

} // namespace

// ---------------------------------------------------------------------------
// Scenario: Happy path: a claim is renewed while its command waits and runs
// ---------------------------------------------------------------------------

TEST_CASE("queue run --claim: a waiting entry renews the claim", "[cmd][agent][queue][hq-claim]") {
  auto const arena = parity::make_arena("qc_wait");
  write_config(arena, k_fast_poll);
  seed_main(arena);
  auto const token = mint_claim(arena);
  gate       hold(arena.cpp_root / "hold.fifo");
  gate       release(arena.cpp_root / "release.fifo");
  release_all guard{.gates = {&hold, &release}};
  spawned     holder;
  spawned     claimed;

  auto const holder_started = arena.cpp_root / "holder.started";
  holder                    = spawn_queue(arena, "holder", blocked(holder_started, hold));
  await_file(holder_started);

  auto const before  = expiry(arena, token);
  auto const started = arena.cpp_root / "claimed.started";
  claimed            = spawn_queue(arena, "claimed", blocked(started, release), {"--claim", token});
  static_cast<void>(await_entry(arena, 2, hq::entry_state::waiting));

  // Two renewals while the entry is still behind the holder.
  auto const first  = await_renewal(arena, token, before);
  auto const second = await_renewal(arena, token, first);
  CHECK(second != first);
  CHECK(entry_seq(require_snapshot(arena), 2)->state == hq::entry_state::waiting);
  CHECK_FALSE(present(started));

  hold.release();
  await_file(started);
  release.release();
  auto const got = finish(claimed);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(finish(holder).code == 0);
}

TEST_CASE("queue run --claim: a running command renews the claim, and the claim is active when it ends",
          "[cmd][agent][queue][hq-claim]") {
  auto const arena = parity::make_arena("qc_run");
  write_config(arena, k_fast_poll);
  seed_main(arena);
  auto const  token = mint_claim(arena);
  gate        release(arena.cpp_root / "release.fifo");
  release_all guard{.gates = {&release}};
  spawned     claimed;

  auto const started = arena.cpp_root / "claimed.started";
  claimed            = spawn_queue(arena, "claimed", blocked(started, release), {"--claim", token});
  await_file(started);
  static_cast<void>(await_entry(arena, 1, hq::entry_state::running));

  // The renewal made when the submitter started is behind us once the command
  // runs; two more come while it does.
  auto const base   = expiry(arena, token);
  auto const first  = await_renewal(arena, token, base);
  auto const second = await_renewal(arena, token, first);
  auto const third  = await_renewal(arena, token, second);
  CHECK(third != second);
  CHECK(entry_seq(require_snapshot(arena), 1)->state == hq::entry_state::running);

  release.release();
  auto const got = finish(claimed);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(claim_field(arena, token, "status") == "active");
  CHECK(claim_field(arena, token, "lease_expires_at") > claim_field(arena, token, "claimed_at"));
}

TEST_CASE("queue run --claim: renewal stops when the command ends", "[cmd][agent][queue][hq-claim]") {
  auto const arena = parity::make_arena("qc_stop");
  write_config(arena, k_fast_poll);
  seed_main(arena);
  auto const token = mint_claim(arena);

  auto const got = run_queue(arena, "stop", sh_command("exit 0"), {"--claim", token});
  INFO("stderr:\n" << got.err);
  REQUIRE(got.code == 0);
  auto const at_end = expiry(arena, token);
  // The interval is two seconds; nothing may renew for well over one.
  std::this_thread::sleep_for(std::chrono::milliseconds(3500));
  CHECK(expiry(arena, token) == at_end);
}

TEST_CASE("queue run: without --claim the main database is never touched", "[cmd][agent][queue][hq-claim]") {
  auto const arena = parity::make_arena("qc_none");
  write_config(arena, k_fast_poll);
  seed_main(arena);
  auto const token  = mint_claim(arena);
  auto const before = claim_field(arena, token, "last_heartbeat_at");

  auto const got = run_queue(arena, "none", sh_command("sleep 3; exit 0"));
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(got.err.empty());
  CHECK(claim_field(arena, token, "last_heartbeat_at") == before);
}

// ---------------------------------------------------------------------------
// Scenarios: a failed renewal is reported and never stops the command
// ---------------------------------------------------------------------------

TEST_CASE("queue run --claim: an unknown claim token is reported and the command still exits 0", "[cmd][agent][queue][hq-claim]") {
  auto const arena = parity::make_arena("qc_bad");
  seed_main(arena);

  auto const got = run_queue(arena, "bad", sh_command("exit 0"), {"--claim", "not-a-token", "--notices"});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 0);
  CHECK(got.out.empty());
  CHECK(warning_count(got.err) == 1);
  CHECK(got.err.find("cannot renew the claim") != std::string::npos);
  CHECK(last_line(got.err) == "queue: entry 1 exited with code 0");
}

TEST_CASE("queue run --claim: a main database that does not exist is reported, not created", "[cmd][agent][queue][hq-claim]") {
  auto const arena = parity::make_arena("qc_nodb");
  REQUIRE_FALSE(present(main_db(arena)));

  auto const got = run_queue(arena, "nodb", sh_command("exit 7"), {"--claim", "0123456789abcdef0123456789abcdef"});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 7);
  CHECK(warning_count(got.err) == 1);
  CHECK_FALSE(present(main_db(arena)));
}

TEST_CASE("queue run --claim: a main database whose schema is ahead is reported and the command runs to its own status",
          "[cmd][agent][queue][hq-claim]") {
  auto const arena = parity::make_arena("qc_ahead");
  {
    auto opened = planar::db::connection::open(main_db(arena).string());
    REQUIRE(opened.has_value());
    REQUIRE(
        opened->execute("create table schema_migrations (version integer primary key, description text not null);").has_value());
    REQUIRE(
        opened->execute("insert into schema_migrations (version, description) values (99999, 'from the future');").has_value());
  }
  auto const before = read_all(main_db(arena));

  auto const got = run_queue(arena, "ahead", sh_command("exit 7"), {"--claim", "0123456789abcdef0123456789abcdef", "--notices"});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 7);
  CHECK(warning_count(got.err) == 1);
  CHECK(last_line(got.err) == "queue: entry 1 exited with code 7");
  CHECK(read_all(main_db(arena)) == before);
  CHECK(require_snapshot(arena).history.size() == 1);
}

TEST_CASE("queue run --claim: a busy main database is reported and the command runs to its own status",
          "[cmd][agent][queue][hq-claim]") {
  auto const arena = parity::make_arena("qc_busy");
  write_config(arena, k_fast_poll);
  seed_main(arena);
  auto const token = mint_claim(arena);

  // The test holds the main database's write lock for the whole run.
  auto locker = planar::db::connection::open(main_db(arena).string());
  REQUIRE(locker.has_value());
  REQUIRE(locker->execute("begin immediate;").has_value());
  auto const before = expiry(arena, token);

  auto const got = run_queue(arena, "busy", sh_command("exit 3"), {"--claim", token});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 3);
  CHECK(warning_count(got.err) == 1);
  REQUIRE(locker->execute("rollback;").has_value());
  CHECK(expiry(arena, token) == before);
}

TEST_CASE("queue run --claim: SIGTERM while waiting still removes the entry and exits 125", "[cmd][agent][queue][hq-claim]") {
  auto const arena = parity::make_arena("qc_sig");
  write_config(arena, k_fast_poll);
  seed_main(arena);
  auto const  token = mint_claim(arena);
  gate        hold(arena.cpp_root / "hold.fifo");
  release_all guard{.gates = {&hold}};
  spawned     holder;
  spawned     claimed;

  auto const holder_started = arena.cpp_root / "holder.started";
  holder                    = spawn_queue(arena, "holder", blocked(holder_started, hold));
  await_file(holder_started);
  auto const marker = arena.cpp_root / "must-not-run";
  claimed           = spawn_queue(arena, "claimed", sh_command("touch \"$1\"", {marker.string()}), {"--claim", token});
  static_cast<void>(await_entry(arena, 2, hq::entry_state::waiting));
  REQUIRE(claimed.still_mine());
  REQUIRE(::kill(static_cast<::pid_t>(claimed.pid), SIGTERM) == 0);

  auto const got = finish(claimed);
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 125);
  CHECK_FALSE(present(marker));
  CHECK(await_history(arena, 2).outcome == hq::history_outcome::cancelled);
  hold.release();
  CHECK(finish(holder).code == 0);
}

TEST_CASE("queue run --claim: the run limit still stops the command at 124", "[cmd][agent][queue][hq-claim]") {
  auto const arena = parity::make_arena("qc_limit");
  write_config(arena, k_fast_poll);
  seed_main(arena);
  auto const token = mint_claim(arena);

  auto const got = run_queue(arena, "limit", sh_command("sleep 30"), {"--claim", token, "--timeout", "1s"});
  INFO("stderr:\n" << got.err);
  CHECK(got.code == 124);
  CHECK(await_history(arena, 1).outcome == hq::history_outcome::timeout);
}

// ---------------------------------------------------------------------------
// Scenario: --claim with --detach: the detached submitter renews
// ---------------------------------------------------------------------------

namespace {

/// @brief Owns a detached submitter: stops it, bounded, when it is still the
/// process its entry named.
struct detached_submitter {
  std::int64_t pid     = 0;
  std::int64_t started = 0;

  detached_submitter()                                     = default;
  detached_submitter(const detached_submitter&)            = delete;
  detached_submitter& operator=(const detached_submitter&) = delete;
  ~detached_submitter() {
    stop();
  }
  void record(const hq::entry& entry) {
    pid     = entry.pid;
    started = entry.pid_started;
  }
  [[nodiscard]] auto still_mine() const -> bool {
    if (pid <= 1) {
      return false;
    }
    auto const now = ident::process_start_time(pid);
    return now && now->has_value() && static_cast<std::int64_t>(**now) == started;
  }
  void stop() {
    if (!still_mine()) {
      return;
    }
    ::kill(static_cast<::pid_t>(pid), SIGTERM);
    if (!await([&] { return !still_mine(); }, std::chrono::seconds(10)) && still_mine()) {
      ::kill(static_cast<::pid_t>(pid), SIGKILL);
      await([&] { return !still_mine(); }, std::chrono::seconds(10));
    }
  }
};

auto ticket_seq(const std::string& out) -> std::int64_t {
  INFO("ticket output:\n" << out);
  auto const first = out.find('\n');
  REQUIRE(first != std::string::npos);
  std::int64_t seq = 0;
  REQUIRE(std::from_chars(out.data(), out.data() + first, seq).ec == std::errc{});
  return seq;
}

auto ticket_path(const std::string& out) -> std::filesystem::path {
  auto const first  = out.find('\n');
  auto const second = out.find('\n', first + 1);
  REQUIRE(second != std::string::npos);
  return out.substr(first + 1, second - first - 1);
}

} // namespace

TEST_CASE("queue run --claim --detach: the detached submitter renews the claim", "[cmd][agent][queue][hq-claim]") {
  auto const arena = parity::make_arena("qc_detach");
  write_config(arena, k_fast_poll);
  seed_main(arena);
  auto const         token = mint_claim(arena);
  gate               release(arena.cpp_root / "release.fifo");
  release_all        guard{.gates = {&release}};
  detached_submitter submitter;

  auto const started = arena.cpp_root / "detached.started";
  auto const got     = run_queue(arena, "detached", blocked(started, release), {"--detach", "--claim", token});
  INFO("stderr:\n" << got.err);
  REQUIRE(got.code == 0);
  auto const seq = ticket_seq(got.out);
  submitter.record(await_entry(arena, seq, hq::entry_state::running));
  await_file(started);

  auto const base   = expiry(arena, token);
  auto const first  = await_renewal(arena, token, base);
  auto const second = await_renewal(arena, token, first);
  CHECK(second != first);

  release.release();
  auto const row = await_history(arena, seq);
  CHECK(row.outcome == hq::history_outcome::exited);
  CHECK(row.exit_code == 0);
  CHECK(claim_field(arena, token, "status") == "active");
}

TEST_CASE("queue run --claim --detach: a failed renewal is reported in the output file", "[cmd][agent][queue][hq-claim]") {
  auto const arena = parity::make_arena("qc_detbad");
  write_config(arena, k_fast_poll);
  seed_main(arena);

  auto const got = run_queue(arena, "detbad", sh_command("exit 5"), {"--detach", "--claim", "not-a-token"});
  INFO("stderr:\n" << got.err);
  REQUIRE(got.code == 0);
  auto const seq  = ticket_seq(got.out);
  auto const path = ticket_path(got.out);
  auto const row  = await_history(arena, seq);
  CHECK(row.exit_code == 5);
  auto const log = read_all(path);
  INFO("log:\n" << log);
  CHECK(warning_count(log) == 1);
}
