// @file log.t.cpp
// @brief Unit tests for `planar.log` (task cpp-cli-output-logging, D11).
//
// Every test builds its own logger via `make_logger()` against an
// in-memory `ostringstream` sink rather than calling `init()` (which
// installs a PROCESS-GLOBAL default logger via spdlog's static registry
// and would make test cases interfere with each other's state).
#include <catch2/catch_test_macros.hpp>
#include <glaze/glaze.hpp> // for the JSON-mode assertions' round-trip parse
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h> // planar.log hands back an opaque shared_ptr<spdlog::logger>; calling its
                           // methods (info/warn/flush/name) needs the complete type, same as any
                           // real caller of make_logger()/scoped() would need.

import std;
import planar.log;

using planar::log::level;
using planar::log::make_logger;
using planar::log::mode;
using planar::log::scoped;

namespace {

// Builds a make_logger() logger writing to a caller-owned ostringstream,
// so the test can inspect the captured text/JSON deterministically.
auto capture_logger(std::string_view name, mode m, std::ostringstream& out, level lvl = level::info) {
  auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(out);
  return make_logger(name, m, sink, lvl);
}

} // namespace

TEST_CASE("make_logger: text mode includes logger name, level, and message", "[log][text]") {
  std::ostringstream out;
  auto               logger = capture_logger("test-logger", mode::text, out);
  logger->info("hello world");
  logger->flush();

  auto text = out.str();
  REQUIRE(text.find("test-logger") != std::string::npos);
  REQUIRE(text.find("info") != std::string::npos);
  REQUIRE(text.find("hello world") != std::string::npos);
}

TEST_CASE("make_logger: json mode emits parseable JSON with ts_ms/level/logger/message fields", "[log][json]") {
  std::ostringstream out;
  auto               logger = capture_logger("test-logger", mode::json, out);
  logger->warn("structured message");
  logger->flush();

  auto text = out.str();
  // Exactly one line (one log call -> one JSON object -> one trailing
  // newline), matching D11's "text/JSON dual mode" line-oriented shape.
  REQUIRE(std::ranges::count(text, '\n') == 1);

  auto parsed = glz::read_json<glz::generic>(text);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->at("logger").get<std::string>() == "test-logger");
  REQUIRE(parsed->at("level").get<std::string>() == "warning");
  REQUIRE(parsed->at("message").get<std::string>() == "structured message");
  REQUIRE(parsed->at("ts_ms").get<double>() > 0.0);
}

// Break-probe: JSON mode must correctly escape a message containing a
// double quote and a newline — a naive string-concatenation formatter
// (instead of going through Glaze's encoder) would emit invalid JSON here.
TEST_CASE("make_logger: json mode escapes adversarial message content", "[log][json]") {
  std::ostringstream out;
  auto               logger = capture_logger("test-logger", mode::json, out);
  logger->info("say \"hi\"\nline2");
  logger->flush();

  auto parsed = glz::read_json<glz::generic>(out.str());
  REQUIRE(parsed.has_value());
  REQUIRE(parsed->at("message").get<std::string>() == "say \"hi\"\nline2");
}

TEST_CASE("make_logger: level filtering — a logger below its threshold emits nothing", "[log][level]") {
  std::ostringstream out;
  auto               logger = capture_logger("test-logger", mode::text, out, level::warn);
  logger->info("should be suppressed");
  logger->flush();
  REQUIRE(out.str().empty());

  logger->warn("should appear");
  logger->flush();
  REQUIRE(out.str().find("should appear") != std::string::npos);
}

TEST_CASE("scoped: same name returns the same underlying logger instance", "[log][scoped]") {
  planar::log::init(mode::text, level::off); // quiets stdout for the rest of this process's tests
  auto a = scoped("cpp-log-t-scoped-idempotency");
  auto b = scoped("cpp-log-t-scoped-idempotency");
  REQUIRE(a.get() == b.get());
}

TEST_CASE("scoped: distinct names return distinct logger instances", "[log][scoped]") {
  planar::log::init(mode::text, level::off);
  auto a = scoped("cpp-log-t-scoped-distinct-a");
  auto b = scoped("cpp-log-t-scoped-distinct-b");
  REQUIRE(a.get() != b.get());
  REQUIRE(a->name() == "cpp-log-t-scoped-distinct-a");
  REQUIRE(b->name() == "cpp-log-t-scoped-distinct-b");
}
