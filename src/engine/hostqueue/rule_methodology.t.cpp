// @file rule_methodology.t.cpp
// @brief Keeps `agents/methodology.md` and the embedded queue rule in step
// (plan 1080, tasks hq-methodology-rule and hq-narrow-quiet-tree). Covers the
// test-spec scenarios "the printed rule and the methodology section agree" and
// "the process check is replaced and the rest is kept".
//
// The methodology file is read from the source tree through
// PLANAR_METHODOLOGY_PATH, set by this directory's CMakeLists.txt. The
// extraction helper is tested against synthetic documents so that a missing
// section or a wrong boundary fails the case instead of passing vacuously.
//
// Include-before-import is deliberate (see core/version.t.cpp / db.t.cpp).
#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.hostqueue;

namespace {

namespace hq = planar::engine::hostqueue;

constexpr std::string_view k_heading = "## Builds and tests go through the host queue";

auto read_file(std::string const& path) -> std::string {
  std::ifstream in(path, std::ios::binary);
  REQUIRE(in.is_open());
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

auto rstrip_newlines(std::string_view s) -> std::string_view {
  while (!s.empty() && s.back() == '\n') {
    s.remove_suffix(1);
  }
  return s;
}

/// @brief The section headed k_heading: from that line to the next `## `
/// heading outside a code fence, or the end of the text. Empty optional when
/// the heading is absent. Trailing newlines are dropped.
auto extract_section(std::string_view doc) -> std::optional<std::string> {
  std::size_t                pos      = 0;
  bool                       in_fence = false;
  std::optional<std::size_t> start;
  while (pos < doc.size()) {
    auto const eol  = doc.find('\n', pos);
    auto const end  = eol == std::string_view::npos ? doc.size() : eol;
    auto const line = doc.substr(pos, end - pos);
    if (line.starts_with("```")) {
      in_fence = !in_fence;
    } else if (!in_fence && line.starts_with("## ")) {
      if (start) {
        return std::string(rstrip_newlines(doc.substr(*start, pos - *start)));
      }
      if (line == k_heading) {
        start = pos;
      }
    }
    pos = eol == std::string_view::npos ? doc.size() : eol + 1;
  }
  if (!start) {
    return std::nullopt;
  }
  return std::string(rstrip_newlines(doc.substr(*start)));
}

auto count_of(std::string_view doc, std::string_view needle) -> std::size_t {
  std::size_t n = 0;
  for (auto at = doc.find(needle); at != std::string_view::npos; at = doc.find(needle, at + needle.size())) {
    ++n;
  }
  return n;
}

} // namespace

TEST_CASE("the methodology section carries the embedded rule text byte for byte", "[hostqueue][methodology]") {
  auto const doc = read_file(PLANAR_METHODOLOGY_PATH);
  REQUIRE(count_of(doc, "\n" + std::string(k_heading) + "\n") == 1);
  auto const section = extract_section(doc);
  REQUIRE(section.has_value());
  auto const rule = rstrip_newlines(hq::queue_rule_text());
  REQUIRE(!rule.empty());
  CHECK(*section == rule);
}

TEST_CASE("the section extractor fails loudly on a missing section and stops at the next H2", "[hostqueue][methodology]") {
  std::string const head(k_heading);
  CHECK(!extract_section("# Doc\n\n## Other\n\ntext\n").has_value());
  // A near-miss heading is not the section.
  CHECK(!extract_section("## Builds and tests go through the host queues\n\nx\n").has_value());
  // It ends at the next H2 and drops the blank line before it.
  CHECK(extract_section("## A\n\nx\n\n" + head + "\n\nbody\n\n## B\n\nmore\n") == std::optional<std::string>{head + "\n\nbody"});
  // It ends at the end of the text when nothing follows.
  CHECK(extract_section("## A\n\n" + head + "\n\nbody\n") == std::optional<std::string>{head + "\n\nbody"});
  // A `## ` line inside a code fence does not end it.
  CHECK(extract_section(head + "\n\n```\n## not a heading\n```\n\n## B\n") ==
        std::optional<std::string>{head + "\n\n```\n## not a heading\n```"});
  // H3 subsections stay inside it.
  CHECK(extract_section(head + "\n\n### Sub\n\nt\n") == std::optional<std::string>{head + "\n\n### Sub\n\nt"});
}

TEST_CASE("the methodology no longer carries the one-build-per-directory process check", "[hostqueue][methodology]") {
  auto const doc = read_file(PLANAR_METHODOLOGY_PATH);
  CHECK(doc.find("pgrep") == std::string::npos);
  CHECK(doc.find("One build at a time per build directory") == std::string::npos);
  // What stays: the failure signatures and the dispatch precondition about a
  // tree in its delivered state.
  CHECK(doc.find("NOT_BUILT") != std::string::npos);
  CHECK(doc.find("a break-probe mutation still applied") != std::string::npos);
  CHECK(doc.find("git status --short") != std::string::npos);
  CHECK(doc.find("go through the host queue") != std::string::npos);
}
