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

TEST_CASE("sha256 matches the PADDING BOUNDARY vectors", "[sha256][6407]") {
  // MOVED HERE at task 6407 from `engine/external/sync.t.cpp`, which tested
  // that bucket's own copy of SHA-256 before this module absorbed it. The
  // three published vectors above do not reach these boundaries, and this is
  // where a length-encoding bug hides:
  //
  //   55 bytes  the largest input whose length field still fits in the first
  //             block
  //   56 bytes  forces a whole extra block of padding
  //   64 bytes  an exact block
  //  119 bytes  one short of two exact blocks
  //
  // These come from Python's `hashlib.sha256` -- an INDEPENDENT
  // implementation, run as
  // `python3 -c "import hashlib; print(hashlib.sha256(b'a'*55).hexdigest())"`
  // -- not from this tree's own output. The original note recorded that a
  // first draft wrote the 55-byte digest from memory and it was wrong; the
  // test caught it. Worth keeping, because it is the reason to distrust a
  // digest constant that was not independently produced.
  CHECK(planar::sha256::hex(std::string(55, 'a')) == "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
  CHECK(planar::sha256::hex(std::string(56, 'a')) == "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
  CHECK(planar::sha256::hex(std::string(64, 'a')) == "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
  CHECK(planar::sha256::hex(std::string(119, 'a')) == "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb");

  // A NUL-bearing input, because the evidence-token format is built entirely
  // out of them: a length-oblivious implementation would stop at the first.
  CHECK(planar::sha256::hex(std::string_view("a\0b", 3)) != planar::sha256::hex("a"));
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
