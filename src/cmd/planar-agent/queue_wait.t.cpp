// Bounded queue wait: a detached ticket is observed as a logical job, and
// repeated in-process invocations release their scoped resources.
#include <catch2/catch_test_macros.hpp>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
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

#include "parity_harness.hpp"
#include "queue_test_store.hpp"

namespace {
namespace parity = planar::cmd::parity;
namespace agent  = planar::cmd::agent;

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

auto highest_fd() -> int {
  int highest = 2;
  for (int fd = 3; fd < 1024; ++fd)
    if (::fcntl(fd, F_GETFD) >= 0)
      highest = fd;
  return highest;
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

TEST_CASE("queue wait: repeated in-process open failures restore descriptors and signal dispositions",
          "[cmd][agent][queue][bqw1123-cli]") {
  auto             arena      = parity::make_arena("queue_wait_cleanup");
  auto const       db         = arena.cpp_root / "missing.db";
  auto const       env        = agent::map_env({{"PLANAR_DB", db.string()}, {"HOME", arena.cpp_root.string()}});
  auto const       before_fds = fd_count();
  struct sigaction int_before{}, term_before{};
  REQUIRE(::sigaction(SIGINT, nullptr, &int_before) == 0);
  REQUIRE(::sigaction(SIGTERM, nullptr, &term_before) == 0);
  planar::cliapp::parsed_args args;
  args.path               = {"queue", "wait"};
  args.positionals["seq"] = "1000001";
  args.flags["--json"]    = {"true"};
  for (int i = 0; i < 32; ++i) {
    std::ostringstream out, err;
    agent::context ctx({"planar-agent", "queue", "wait"}, env, arena.cpp_root, std::make_shared<agent::database>(db, err), out,
                       err);
    auto           result = agent::handlers::queue_wait(ctx, args);
    REQUIRE(std::holds_alternative<agent::exit_status>(result));
    CHECK(std::get<agent::exit_status>(result).code == 125);
    CHECK(out.str().find("\"tag\":\"store_unreachable\"") != std::string::npos);
    CHECK(fd_count() == before_fds);
    struct sigaction int_after{}, term_after{};
    REQUIRE(::sigaction(SIGINT, nullptr, &int_after) == 0);
    REQUIRE(::sigaction(SIGTERM, nullptr, &term_after) == 0);
    CHECK(int_after.sa_handler == int_before.sa_handler);
    CHECK(term_after.sa_handler == term_before.sa_handler);
  }
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

TEST_CASE("queue wait: failed pipe setup releases partial resources in process", "[cmd][agent][queue][bqw1123-cli]") {
  auto          arena = parity::make_arena("queue_wait_pipe_fail");
  auto const    db    = arena.cpp_root / "missing.db";
  auto const    env   = agent::map_env({{"PLANAR_DB", db.string()}, {"HOME", arena.cpp_root.string()}});
  struct rlimit prior{};
  REQUIRE(::getrlimit(RLIMIT_NOFILE, &prior) == 0);
  struct restore {
    struct rlimit prior;
    ~restore() {
      ::setrlimit(RLIMIT_NOFILE, &prior);
    }
  } guard{prior};
  struct rlimit limited = prior;
  limited.rlim_cur      = static_cast<rlim_t>(highest_fd() + 1);
  REQUIRE(::setrlimit(RLIMIT_NOFILE, &limited) == 0);
  auto const                  before = fd_count();
  planar::cliapp::parsed_args args;
  args.path               = {"queue", "wait"};
  args.positionals["seq"] = "1000001";
  for (int i = 0; i < 3; ++i) {
    std::ostringstream out, err;
    agent::context ctx({"planar-agent", "queue", "wait"}, env, arena.cpp_root, std::make_shared<agent::database>(db, err), out,
                       err);
    auto           result = agent::handlers::queue_wait(ctx, args);
    REQUIRE(std::holds_alternative<agent::exit_status>(result));
    CHECK(std::get<agent::exit_status>(result).code == 125);
    CHECK(out.str().find("signal_setup_failed") != std::string::npos);
    CHECK(fd_count() == before);
  }
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
