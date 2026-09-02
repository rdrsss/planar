#include <catch2/catch_test_macros.hpp>

import std;
import planar.sha256;

TEST_CASE("sha256 matches FIPS and embedded-NUL vectors", "[sha256]") {
  CHECK(planar::sha256::hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(planar::sha256::hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(planar::sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  std::string nul{"a\0b", 3};
  CHECK(planar::sha256::hex(nul) == "59b271ae1bbcb1d31d41929817f4b16fb439eb4f31520b5ad1d5ce98920a7138");
}

TEST_CASE("sha256 matches the canonical synthesis request stream", "[sha256][synthesize]") {
  std::string material;
  auto        rec = [&](std::initializer_list<std::string_view> parts) {
    for (auto part : parts) {
      material.append(part);
      material.push_back('\0');
    }
  };
  auto const readme = "# Probe Project\n\nAn oracle synthesis probe.\n";
  auto const guide  = "# Guide\n\nKeep the probe hermetic.\n";
  rec({"repo_slug", "repo"});
  rec({"readme", readme});
  rec({"docs", "1"});
  rec({"README.md", readme});
  rec({"guide_files", "1"});
  rec({"AGENTS.md", guide});
  rec({"tree_summary", "3"});
  rec({"AGENTS.md"});
  rec({"README.md"});
  rec({"src/"});
  rec({"git_log", "0"});
  rec({"layout", "mixed", ""});
  rec({"areas", "1"});
  rec({"src/main.zig", "main.zig"});
  rec({"source_files", "1"});
  rec({"test_files", "0"});
  rec({"greenfield", "0"});
  REQUIRE(material.size() == 323);
  CHECK(planar::sha256::hex(material) == "05f3394dc1d7f09774bafd7c98385492493f8e7938e589987e0e2fb1ebc0ada5");
}
