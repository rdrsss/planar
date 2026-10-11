// Bounded queue wait: a detached ticket is observed as a logical job, and
// repeated in-process invocations release their scoped resources.
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

import std;
import planar.cliapp.args;
import planar.cmd.planar_agent.context;
import planar.cmd.planar_agent.exit;
import planar.cmd.planar_agent.handler;
import planar.cmd.planar_agent.handlers.queue.wait;
import planar.db;
import planar.db.migrate;
import planar.db.migrations;
import planar.engine.hostqueue;
import planar.process.identity;

#include "parity_harness.hpp"
#include "queue_test_store.hpp"

namespace {
namespace parity = planar::cmd::parity;
namespace agent  = planar::cmd::agent;
namespace hq     = planar::engine::hostqueue;
namespace ident  = planar::process::identity;

auto agent_bin() -> std::filesystem::path {
  return std::filesystem::path{PLANAR_CPP_BIN};
}

auto run(const parity::arena& arena, std::vector<std::string> args, std::string_view tag) -> parity::capture {
  return parity::run_pinned(agent_bin(), args, arena.cpp_root, tag);
}

auto fd_count() -> int {
  int count = 0;
  for (int fd = 0; fd < 256; ++fd)
    if (::fcntl(fd, F_GETFD) >= 0)
      ++count;
  return count;
}

struct invocation {
  int         code;
  std::string out;
  std::string err;
};

auto wait_in_process(const parity::arena& arena, std::int64_t seq, std::string_view timeout = "2s",
                     const std::function<bool()>& after_first_install = {}) -> invocation {
  auto const                  db  = arena.cpp_root / "planar.db";
  auto const                  env = agent::map_env({{"PLANAR_DB", db.string()}, {"HOME", arena.cpp_root.string()}});
  planar::cliapp::parsed_args args;
  args.path               = {"queue", "wait"};
  args.positionals["seq"] = std::to_string(seq);
  args.flags["--timeout"] = {std::string{timeout}};
  args.flags["--json"]    = {"true"};
  std::ostringstream out, err;
  agent::context     ctx({"planar-agent", "queue", "wait"}, env, arena.cpp_root, std::make_shared<agent::database>(db, err), out,
                         err);
  auto               result = agent::handlers::queue_wait_with_signal_setup_hook(ctx, args, after_first_install);
  auto*              status = std::get_if<agent::exit_status>(&result);
  return {.code = status ? status->code : -1, .out = out.str(), .err = err.str()};
}

auto seed_waiting(const std::filesystem::path& db, bool stale) -> std::int64_t {
  auto writer = planar::cmd::qfix::open_store(db);
  REQUIRE(writer.has_value());
  ident::system_clock clock;
  auto                now = clock.monotonic_ms();
  REQUIRE(now.has_value());
  auto seq = hq::enqueue(*writer, hq::enqueue_request{.host_id        = "foreign-test-host",
                                                      .pid            = 9'999'999,
                                                      .pid_started    = 1,
                                                      .cwd            = "/scratch",
                                                      .argv           = {"true"},
                                                      .enqueued_at    = clock.wall_ms(),
                                                      .refreshed_mono = stale ? 0 : *now});
  REQUIRE(seq.has_value());
  return *seq;
}

auto checkpoint_clear(const std::filesystem::path& db) -> bool {
  auto writer = planar::cmd::qfix::open_store(db);
  REQUIRE(writer.has_value());
  auto checkpoint = writer->prepare("pragma wal_checkpoint(truncate)");
  REQUIRE(checkpoint.has_value());
  auto row = checkpoint->step();
  REQUIRE(row.has_value());
  REQUIRE(*row == planar::db::step_result::row);
  return checkpoint->column_int64(0) == 0 && checkpoint->column_int64(1) == checkpoint->column_int64(2);
}

void noop_signal(int) {
}
} // namespace

TEST_CASE("queue wait: detached command completion keeps the ticket and recorded outcome", "[cmd][agent][queue][bqw1123-cli]") {
  auto arena = parity::make_arena("queue_wait_completed");
  planar::cmd::qfix::head_store(arena.cpp_root / "planar.db");
  auto submitted = run(arena, {"queue", "run", "--detach", "--", "true"}, "submit");
  REQUIRE(submitted.code == 0);
  auto seq = submitted.out.substr(0, submitted.out.find('\n'));
  REQUIRE_FALSE(seq.empty());
  auto waited = run(arena, {"queue", "wait", seq, "--timeout", "20s", "--json"}, "wait");
  INFO(waited.out << waited.err);
  REQUIRE(waited.code == 0);
  CHECK(waited.out.find("\"wait_reason\":\"completed\"") != std::string::npos);
  CHECK(waited.out.find("\"outcome\":\"exited\"") != std::string::npos);
  CHECK(waited.out.find("\"result_exit_code\":0") != std::string::npos);
  auto status = run(arena, {"queue", "status", seq, "--json"}, "status");
  CHECK(status.code == 0);
  CHECK(status.out.find("\"outcome\":\"exited\"") != std::string::npos);
}

TEST_CASE("queue wait: invalid input and absent history have distinct reasons", "[cmd][agent][queue][bqw1123-cli]") {
  auto arena = parity::make_arena("queue_wait_input");
  planar::cmd::qfix::head_store(arena.cpp_root / "planar.db");
  for (auto const& seq : {"0", "-1", "1.5", "9223372036854775808"}) {
    auto result = run(arena, {"queue", "wait", seq, "--json"}, seq);
    CHECK(result.code == 2);
    CHECK(result.out.find("\"tag\":\"invalid_input\"") != std::string::npos);
  }
  auto missing = run(arena, {"queue", "wait", "9000000", "--json"}, "missing");
  CHECK(missing.code == 1);
  CHECK(missing.out.find("\"wait_reason\":\"history_unavailable\"") != std::string::npos);
  CHECK(missing.out.find("\"tag\":\"not_found\"") != std::string::npos);
}

TEST_CASE("queue wait: repeated real handler outcomes release descriptors, dispositions and WAL readers",
          "[cmd][agent][queue][bqw1123-cli]") {
  auto arena = parity::make_arena("queue_wait_cleanup");
  auto db    = arena.cpp_root / "planar.db";
  planar::cmd::qfix::head_store(db);
  auto completed = seed_waiting(db, false);
  {
    auto writer = planar::cmd::qfix::open_store(db);
    REQUIRE(writer.has_value());
    auto ended =
        hq::end_entry(*writer, completed,
                      hq::end_request{.outcome = hq::history_outcome::exited, .exit_code = 0, .ended_at = 1'759'000'001'000});
    REQUIRE(ended.has_value());
    CHECK(*ended == hq::end_result::ended);
  }
  auto             stalled    = seed_waiting(db, true);
  auto             pending    = seed_waiting(db, false);
  auto const       before_fds = fd_count();
  struct sigaction int_before{}, term_before{};
  REQUIRE(::sigaction(SIGINT, nullptr, &int_before) == 0);
  REQUIRE(::sigaction(SIGTERM, nullptr, &term_before) == 0);
  for (int i = 0; i < 3; ++i) {
    auto done = wait_in_process(arena, completed);
    CHECK(done.code == 0);
    CHECK(done.out.find("\"wait_reason\":\"completed\"") != std::string::npos);
    auto missing = wait_in_process(arena, 9'000'000);
    CHECK(missing.code == 1);
    CHECK(missing.out.find("\"wait_reason\":\"history_unavailable\"") != std::string::npos);
    auto dead = wait_in_process(arena, stalled);
    CHECK(dead.code == 125);
    CHECK(dead.out.find("\"wait_reason\":\"stalled\"") != std::string::npos);
    auto expired = wait_in_process(arena, pending, "1ms");
    CHECK(expired.code == 124);
    CHECK(expired.out.find("\"wait_reason\":\"timed_out\"") != std::string::npos);
    CHECK(fd_count() == before_fds);
    CHECK(checkpoint_clear(db));
    struct sigaction int_after{}, term_after{};
    REQUIRE(::sigaction(SIGINT, nullptr, &int_after) == 0);
    REQUIRE(::sigaction(SIGTERM, nullptr, &term_after) == 0);
    CHECK(int_after.sa_handler == int_before.sa_handler);
    CHECK(term_after.sa_handler == term_before.sa_handler);
  }
  CHECK(planar::cmd::qfix::queue_row_count(db) == 3);
}

TEST_CASE("queue wait: inherited ignored SIGINT stays ignored across invocation", "[cmd][agent][queue][bqw1123-cli]") {
  auto             arena = parity::make_arena("queue_wait_ignored");
  auto const       db    = arena.cpp_root / "missing.db";
  struct sigaction old{}, ignored{};
  REQUIRE(::sigaction(SIGINT, nullptr, &old) == 0);
  ignored.sa_handler = SIG_IGN;
  sigemptyset(&ignored.sa_mask);
  REQUIRE(::sigaction(SIGINT, &ignored, nullptr) == 0);
  struct restore {
    struct sigaction old;
    ~restore() {
      ::sigaction(SIGINT, &old, nullptr);
    }
  } guard{old};
  auto const                  env = agent::map_env({{"PLANAR_DB", db.string()}, {"HOME", arena.cpp_root.string()}});
  planar::cliapp::parsed_args args;
  args.path               = {"queue", "wait"};
  args.positionals["seq"] = "1000001";
  for (int i = 0; i < 4; ++i) {
    std::ostringstream out, err;
    agent::context ctx({"planar-agent", "queue", "wait"}, env, arena.cpp_root, std::make_shared<agent::database>(db, err), out,
                       err);
    auto           result = agent::handlers::queue_wait(ctx, args);
    REQUIRE(std::holds_alternative<agent::exit_status>(result));
    struct sigaction current{};
    REQUIRE(::sigaction(SIGINT, nullptr, &current) == 0);
    CHECK(current.sa_handler == SIG_IGN);
  }
}

TEST_CASE("queue wait: failure after installing the first signal handler releases its acquired resources",
          "[cmd][agent][queue][bqw1123-cli]") {
  auto arena = parity::make_arena("queue_wait_partial_signal");
  auto db    = arena.cpp_root / "planar.db";
  planar::cmd::qfix::head_store(db);
  struct sigaction old{}, custom{};
  REQUIRE(::sigaction(SIGINT, nullptr, &old) == 0);
  custom.sa_handler = noop_signal;
  sigemptyset(&custom.sa_mask);
  REQUIRE(::sigaction(SIGINT, &custom, nullptr) == 0);
  struct restore {
    struct sigaction old;
    ~restore() {
      ::sigaction(SIGINT, &old, nullptr);
    }
  } guard{old};
  auto const       before = fd_count();
  struct sigaction term_before{};
  REQUIRE(::sigaction(SIGTERM, nullptr, &term_before) == 0);
  for (int i = 0; i < 3; ++i) {
    bool acquired = false;
    auto failed   = wait_in_process(arena, 9'000'000, "2s", [&] {
      struct sigaction active{};
      acquired = fd_count() == before + 2 && ::sigaction(SIGINT, nullptr, &active) == 0 && active.sa_handler != noop_signal;
      return false;
    });
    CHECK(acquired);
    CHECK(failed.code == 125);
    CHECK(failed.out.find("signal_setup_failed") != std::string::npos);
    CHECK(fd_count() == before);
    struct sigaction int_after{}, term_after{};
    REQUIRE(::sigaction(SIGINT, nullptr, &int_after) == 0);
    REQUIRE(::sigaction(SIGTERM, nullptr, &term_after) == 0);
    CHECK(int_after.sa_handler == noop_signal);
    CHECK(term_after.sa_handler == term_before.sa_handler);
    CHECK(checkpoint_clear(db));
  }
  auto next = wait_in_process(arena, 9'000'000);
  CHECK(next.code == 1);
}

TEST_CASE("queue wait: a live observation holds no WAL read snapshot between samples", "[cmd][agent][queue][bqw1123-cli]") {
  auto arena = parity::make_arena("queue_wait_wal_progress");
  auto db    = arena.cpp_root / "planar.db";
  planar::cmd::qfix::head_store(db);
  auto              pending = seed_waiting(db, false);
  auto              before  = fd_count();
  std::atomic<bool> finished{false};
  invocation        observed{};
  std::jthread      observer([&] {
    observed = wait_in_process(arena, pending, "2s");
    finished.store(true);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds{150});
  CHECK_FALSE(finished.load());
  {
    auto writer = planar::cmd::qfix::open_store(db);
    REQUIRE(writer.has_value());
    CHECK(writer->execute("update queue_schema set description = 'writer-progress' where version = 1").has_value());
    {
      auto checkpoint = writer->prepare("pragma wal_checkpoint(truncate)");
      REQUIRE(checkpoint.has_value());
      auto row = checkpoint->step();
      REQUIRE(row.has_value());
      REQUIRE(*row == planar::db::step_result::row);
      CHECK(checkpoint->column_int64(0) == 0);
      CHECK(checkpoint->column_int64(1) == checkpoint->column_int64(2));
    }
    CHECK(std::filesystem::file_size(db.string() + "-wal") == 0);
  }
  observer.join();
  CHECK(observed.code == 124);
  CHECK(observed.out.find("\"wait_reason\":\"timed_out\"") != std::string::npos);
  CHECK(fd_count() == before);
  CHECK(checkpoint_clear(db));
}

TEST_CASE("queue wait: SIGINT and SIGTERM during contended store reads release in-process resources",
          "[cmd][agent][queue][bqw1123-cli]") {
  auto arena = parity::make_arena("queue_wait_busy_signals");
  auto db    = arena.cpp_root / "planar.db";
  planar::cmd::qfix::head_store(db);
  auto writer = planar::cmd::qfix::open_store(db);
  REQUIRE(writer.has_value());
  REQUIRE(writer->execute("pragma journal_mode = delete").has_value());
  struct sigaction int_before{}, term_before{}, custom{};
  REQUIRE(::sigaction(SIGINT, nullptr, &int_before) == 0);
  REQUIRE(::sigaction(SIGTERM, nullptr, &term_before) == 0);
  custom.sa_handler = noop_signal;
  sigemptyset(&custom.sa_mask);
  REQUIRE(::sigaction(SIGINT, &custom, nullptr) == 0);
  REQUIRE(::sigaction(SIGTERM, &custom, nullptr) == 0);
  struct restore {
    struct sigaction int_before, term_before;
    ~restore() {
      ::sigaction(SIGINT, &int_before, nullptr);
      ::sigaction(SIGTERM, &term_before, nullptr);
    }
  } guard{int_before, term_before};
  auto before = fd_count();
  for (int signal : {SIGINT, SIGTERM}) {
    REQUIRE(writer->execute("begin exclusive").has_value());
    invocation        observed{};
    std::atomic<bool> finished{false};
    std::jthread      observer([&] {
      observed = wait_in_process(arena, 9'000'000, "5s");
      finished.store(true);
    });
    bool              installed = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
      struct sigaction current{};
      REQUIRE(::sigaction(signal, nullptr, &current) == 0);
      if (current.sa_handler != noop_signal) {
        installed = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    CHECK(installed);
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    CHECK_FALSE(finished.load());
    if (installed)
      REQUIRE(::kill(::getpid(), signal) == 0);
    observer.join();
    CHECK(observed.code == 128 + signal);
    CHECK(observed.out.find("\"wait_reason\":\"interrupted\"") != std::string::npos);
    CHECK(writer->execute("rollback").has_value());
    CHECK(fd_count() == before);
    struct sigaction int_after{}, term_after{};
    REQUIRE(::sigaction(SIGINT, nullptr, &int_after) == 0);
    REQUIRE(::sigaction(SIGTERM, nullptr, &term_after) == 0);
    CHECK(int_after.sa_handler == noop_signal);
    CHECK(term_after.sa_handler == noop_signal);
  }
  CHECK(checkpoint_clear(db));
  auto next = wait_in_process(arena, 9'000'000);
  CHECK(next.code == 1);
}

TEST_CASE("queue wait: SIGTERM promptly interrupts only the observer", "[cmd][agent][queue][bqw1123-cli]") {
  auto arena = parity::make_arena("queue_wait_signal");
  planar::cmd::qfix::head_store(arena.cpp_root / "planar.db");
  auto submitted = run(arena, {"queue", "run", "--detach", "--", "sleep", "10"}, "submit");
  REQUIRE(submitted.code == 0);
  auto seq = submitted.out.substr(0, submitted.out.find('\n'));
  struct cancel_on_exit {
    const parity::arena& arena;
    std::string          seq;
    ~cancel_on_exit() {
      static_cast<void>(run(arena, {"queue", "cancel", seq}, "cleanup"));
    }
  } cleanup{arena, seq};

  auto output = arena.cpp_root / "observer.out";
  auto error  = arena.cpp_root / "observer.err";
  auto pid    = ::fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    auto outfd = ::open(output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    auto errfd = ::open(error.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (outfd < 0 || errfd < 0 || ::dup2(outfd, STDOUT_FILENO) < 0 || ::dup2(errfd, STDERR_FILENO) < 0)
      ::_exit(99);
    ::close(outfd);
    ::close(errfd);
    ::setenv("PLANAR_DB", (arena.cpp_root / "planar.db").c_str(), 1);
    ::setenv("HOME", (arena.cpp_root / "fakehome").c_str(), 1);
    ::unsetenv("PLANAR_QUEUE_SLOT");
    auto bin = agent_bin().string();
    ::execl(bin.c_str(), bin.c_str(), "queue", "wait", seq.c_str(), "--timeout", "5s", "--json", nullptr);
    ::_exit(98);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds{250});
  auto sent_at = std::chrono::steady_clock::now();
  REQUIRE(::kill(pid, SIGTERM) == 0);
  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  auto elapsed = std::chrono::steady_clock::now() - sent_at;
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 143);
  CHECK(elapsed < std::chrono::seconds{2});
  auto result = parity::read_all(output);
  CHECK(result.find("\"wait_reason\":\"interrupted\"") != std::string::npos);
  auto job = run(arena, {"queue", "status", seq, "--json"}, "after_signal");
  CHECK(job.code == 0);
  CHECK(job.out.find("\"state\":\"running\"") != std::string::npos);
}
