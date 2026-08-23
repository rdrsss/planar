// @file render.t.cpp
// @brief Unit tests for `planar.engine.runs.render` (plan 996, task 6095).
//
// ORACLE PROVENANCE. Every expected string in this file is a VERBATIM byte
// capture from the Zig binary, never from `--help` and never from reading the
// Zig source. Because `runs.started_at` / `run_events.created_at` are
// schema-stamped with `now`, the render fixtures were pinned by writing rows
// with FIXED timestamps straight into a scratch database and then rendering
// THAT, so the captures are byte-stable and re-runnable:
//
//   mkdir -p /tmp/fx/repo && cd /tmp/fx/repo && git init -q .
//   export PLANAR_DB=/tmp/fx/p.db PLANAR_CONFIG_PATH=/tmp/fx/p.toml
//   ./zig/zig-out/bin/planar init
//   sqlite3 /tmp/fx/p.db "
//     insert into plans (scope_kind,title,slug,status,created_at,updated_at)
//       values ('global','P','p','draft','2020-01-01T00:00:00.000Z','2020-01-01T00:00:00.000Z');
//     insert into runs (run_uid,plan_id,arm,base_sha,config_hash,config_json,
//                       corpus_repo,status,started_at,ended_at) values
//       ('full-uid',1,'strict','deadbeef','ch1','{\"k\":1}','/corpus',
//        'completed','2020-01-01T00:00:00.000Z','2020-01-02T00:00:00.000Z'),
//       ('min-uid',1,'op','','',null,null,'running',
//        '2020-01-03T00:00:00.000Z',null);
//     insert into run_events (run_id,seq,kind,payload,created_at) values
//       (1,1,'dispatch',null,'2020-01-01T00:00:01.000Z'),
//       (1,2,'result','{\"a\":1}','2020-01-01T00:00:02.000Z'),
//       (1,9,'after',null,'2020-01-01T00:00:03.000Z');
//     insert into run_touches (run_id,task_id,path,kind,created_at) values
//       (1,1,'src/a.zig','declared','2020-01-01T00:00:04.000Z'),
//       (1,2,'src/b.zig','actual','2020-01-01T00:00:05.000Z');"
//
// The eight resulting captures (`repr()`-dumped so trailing spaces and
// newlines are visible) are transcribed into the k_* constants below.
//
// The escape fixture came from a live run instead, since it exercises the
// argument path:
//
//   $Z bench start 'q"uid\back' --plan 1 --arm 'a"rm<TAB>tab' \
//        --base-sha $'sha\nnl' --config-hash 'h€ü' --corpus-repo 'c/r'
//   $Z bench event 'q"uid\back' --kind $'k\x01ctl' --seq 1
//   $Z bench show 'q"uid\back' --json
//     -> ..."run_uid":"q\"uid\\back","arm":"a\"rm\ttab","base_sha":"sha\nnl",
//           "config_hash":"h€ü","corpus_repo":"c/r",..."kind":"k\u0001ctl"...
//
//   (the 0x01 byte came back as a LOWERCASE \u0001 escape; `h€ü` passed
//    through as raw UTF-8; `c/r` kept its bare slash)
//
// And the three write-leaf envelopes, plus the error strings, from the
// lifecycle probe session transcribed in lifecycle.t.cpp's header:
//
//   $Z run start --plan 1 --json
//     -> {"run_uid":"c5678087d1831bc7fe1e47a35d35f5f3","plan_id":1,"arm":"op"}
//   $Z run start --plan 1 --workflow wf1 --json  -> ..."arm":"wf1"}
//   $Z run event <uid> --kind step2   -> {"run_uid":"<uid>","seq":3,"kind":"step2"}
//   $Z run finish <uid> --status completed
//                                     -> {"run_uid":"<uid>","status":"completed"}
//   $Z bench start r1 ...             -> stdout "r1\n"        [NOT json]
//   $Z bench event|touch|finish ...   -> stdout "ok\n"        [NOT json]
//
// `bench harvest` has no renderer here: the leaf is not ported (see
// CMakeLists.txt's cut list).

#include <catch2/catch_test_macros.hpp>

import std;
import planar.engine.runs.lifecycle;
import planar.engine.runs.render;

namespace {

namespace rl = planar::engine::runs::lifecycle;
namespace rr = planar::engine::runs::render;

// --- Oracle captures, verbatim ------------------------------------------

constexpr std::string_view k_bench_show_full_json =
    R"({"id":1,"run_uid":"full-uid","plan_id":1,"arm":"strict","base_sha":"deadbeef","config_hash":"ch1",)"
    R"("config_json":{"k":1},"corpus_repo":"/corpus","status":"completed",)"
    R"("started_at":"2020-01-01T00:00:00.000Z","ended_at":"2020-01-02T00:00:00.000Z","events":[)"
    R"({"id":1,"seq":1,"kind":"dispatch","payload":null,"created_at":"2020-01-01T00:00:01.000Z"},)"
    R"({"id":2,"seq":2,"kind":"result","payload":{"a":1},"created_at":"2020-01-01T00:00:02.000Z"},)"
    R"({"id":3,"seq":9,"kind":"after","payload":null,"created_at":"2020-01-01T00:00:03.000Z"}],"touches":[)"
    R"({"id":1,"task_id":1,"path":"src/a.zig","kind":"declared","created_at":"2020-01-01T00:00:04.000Z"},)"
    R"({"id":2,"task_id":2,"path":"src/b.zig","kind":"actual","created_at":"2020-01-01T00:00:05.000Z"}]})";

// Note the two blank lines before each section header, and that `config_json`
// has NO text row at all even when set.
constexpr std::string_view k_bench_show_full_text = "run:         full-uid\n"
                                                    "plan_id:     1\n"
                                                    "arm:         strict\n"
                                                    "status:      completed\n"
                                                    "base_sha:    deadbeef\n"
                                                    "config_hash: ch1\n"
                                                    "corpus_repo: /corpus\n"
                                                    "started_at:  2020-01-01T00:00:00.000Z\n"
                                                    "ended_at:    2020-01-02T00:00:00.000Z\n"
                                                    "\n"
                                                    "events (3):\n"
                                                    "  [1] dispatch\n"
                                                    R"(  [2] result: {"a":1})"
                                                    "\n"
                                                    "  [9] after\n"
                                                    "\n"
                                                    "touches (2):\n"
                                                    "  task=1 path=src/a.zig kind=declared\n"
                                                    "  task=2 path=src/b.zig kind=actual\n";

constexpr std::string_view k_run_show_full_json =
    R"({"id":1,"run_uid":"full-uid","plan_id":1,"arm":"strict","status":"completed",)"
    R"("started_at":"2020-01-01T00:00:00.000Z","ended_at":"2020-01-02T00:00:00.000Z","events":[)"
    R"({"id":1,"seq":1,"kind":"dispatch","payload":null,"created_at":"2020-01-01T00:00:01.000Z"},)"
    R"({"id":2,"seq":2,"kind":"result","payload":{"a":1},"created_at":"2020-01-01T00:00:02.000Z"},)"
    R"({"id":3,"seq":9,"kind":"after","payload":null,"created_at":"2020-01-01T00:00:03.000Z"}]})";

constexpr std::string_view k_run_show_full_text = "run:        full-uid\n"
                                                  "plan_id:    1\n"
                                                  "arm:        strict\n"
                                                  "status:     completed\n"
                                                  "started_at: 2020-01-01T00:00:00.000Z\n"
                                                  "ended_at:   2020-01-02T00:00:00.000Z\n"
                                                  "\n"
                                                  "events (3):\n"
                                                  "  [1] dispatch\n"
                                                  R"(  [2] result: {"a":1})"
                                                  "\n"
                                                  "  [9] after\n";

constexpr std::string_view k_bench_show_min_json =
    R"({"id":2,"run_uid":"min-uid","plan_id":1,"arm":"op","base_sha":"","config_hash":"",)"
    R"("config_json":null,"corpus_repo":null,"status":"running",)"
    R"("started_at":"2020-01-03T00:00:00.000Z","ended_at":null,"events":[],"touches":[]})";

// The empty base_sha / config_hash rows keep their label padding, so both
// lines end in trailing whitespace. That is the oracle's byte sequence:
// b'base_sha:    \nconfig_hash: \n'.
constexpr std::string_view k_bench_show_min_text = "run:         min-uid\n"
                                                   "plan_id:     1\n"
                                                   "arm:         op\n"
                                                   "status:      running\n"
                                                   "base_sha:    \n"
                                                   "config_hash: \n"
                                                   "started_at:  2020-01-03T00:00:00.000Z\n"
                                                   "\n"
                                                   "events (0):\n"
                                                   "\n"
                                                   "touches (0):\n";

constexpr std::string_view k_run_show_min_json = R"({"id":2,"run_uid":"min-uid","plan_id":1,"arm":"op","status":"running",)"
                                                 R"("started_at":"2020-01-03T00:00:00.000Z","ended_at":null,"events":[]})";

constexpr std::string_view k_run_show_min_text = "run:        min-uid\n"
                                                 "plan_id:    1\n"
                                                 "arm:        op\n"
                                                 "status:     running\n"
                                                 "started_at: 2020-01-03T00:00:00.000Z\n"
                                                 "\n"
                                                 "events (0):\n";

// --- Fixtures matching the captured rows exactly -------------------------

auto full_run() -> rl::run {
  return rl::run{
      .id          = 1,
      .run_uid     = "full-uid",
      .plan_id     = 1,
      .arm         = "strict",
      .base_sha    = "deadbeef",
      .config_hash = "ch1",
      .config_json = std::string{R"({"k":1})"},
      .corpus_repo = std::string{"/corpus"},
      .status      = "completed",
      .started_at  = "2020-01-01T00:00:00.000Z",
      .ended_at    = std::string{"2020-01-02T00:00:00.000Z"},
  };
}

auto min_run() -> rl::run {
  return rl::run{
      .id          = 2,
      .run_uid     = "min-uid",
      .plan_id     = 1,
      .arm         = "op",
      .base_sha    = "",
      .config_hash = "",
      .config_json = std::nullopt,
      .corpus_repo = std::nullopt,
      .status      = "running",
      .started_at  = "2020-01-03T00:00:00.000Z",
      .ended_at    = std::nullopt,
  };
}

auto full_events() -> std::vector<rl::event_row> {
  return {
      rl::event_row{
          .id = 1, .run_id = 1, .seq = 1, .kind = "dispatch", .payload = std::nullopt, .created_at = "2020-01-01T00:00:01.000Z"},
      rl::event_row{.id         = 2,
                    .run_id     = 1,
                    .seq        = 2,
                    .kind       = "result",
                    .payload    = std::string{R"({"a":1})"},
                    .created_at = "2020-01-01T00:00:02.000Z"},
      rl::event_row{
          .id = 3, .run_id = 1, .seq = 9, .kind = "after", .payload = std::nullopt, .created_at = "2020-01-01T00:00:03.000Z"},
  };
}

auto full_touches() -> std::vector<rl::touch_row> {
  return {
      rl::touch_row{.id         = 1,
                    .run_id     = 1,
                    .task_id    = 1,
                    .path       = "src/a.zig",
                    .kind_      = rl::touch_kind::declared,
                    .created_at = "2020-01-01T00:00:04.000Z"},
      rl::touch_row{.id         = 2,
                    .run_id     = 1,
                    .task_id    = 2,
                    .path       = "src/b.zig",
                    .kind_      = rl::touch_kind::actual,
                    .created_at = "2020-01-01T00:00:05.000Z"},
  };
}

} // namespace

TEST_CASE("runs.render: bench show --json matches the oracle byte for byte", "[runs]") {
  const auto events  = full_events();
  const auto touches = full_touches();
  REQUIRE(rr::render_bench_show_json(full_run(), events, touches) == k_bench_show_full_json);
}

TEST_CASE("runs.render: bench show text matches the oracle byte for byte", "[runs]") {
  const auto events  = full_events();
  const auto touches = full_touches();
  REQUIRE(rr::render_bench_show_text(full_run(), events, touches) == k_bench_show_full_text);
}

TEST_CASE("runs.render: run show --json omits the measurement fields entirely", "[runs]") {
  const auto events = full_events();
  const auto out    = rr::render_run_show_json(full_run(), events);
  REQUIRE(out == k_run_show_full_json);
  // Stated positively above; stated as absence here, because the whole point
  // of the two renderers being separate is that these five keys must NOT
  // leak onto the operational surface even though the row carries them.
  REQUIRE(out.find("base_sha") == std::string::npos);
  REQUIRE(out.find("config_hash") == std::string::npos);
  REQUIRE(out.find("config_json") == std::string::npos);
  REQUIRE(out.find("corpus_repo") == std::string::npos);
  REQUIRE(out.find("touches") == std::string::npos);
}

TEST_CASE("runs.render: run show text matches the oracle byte for byte", "[runs]") {
  const auto events = full_events();
  REQUIRE(rr::render_run_show_text(full_run(), events) == k_run_show_full_text);
}

TEST_CASE("runs.render: bench and run text renderers pad to DIFFERENT widths", "[runs]") {
  const auto events  = full_events();
  const auto touches = full_touches();
  const auto bench   = rr::render_bench_show_text(full_run(), events, touches);
  const auto run     = rr::render_run_show_text(full_run(), events);
  // bench pads to 13 ("config_hash: "), run to 12 ("started_at: "). A single
  // parameterized renderer would make one of these wrong.
  REQUIRE(bench.starts_with("run:         full-uid\n"));
  REQUIRE(run.starts_with("run:        full-uid\n"));
}

TEST_CASE("runs.render: an empty run renders empty arrays and omits the unset rows", "[runs]") {
  REQUIRE(rr::render_bench_show_json(min_run(), {}, {}) == k_bench_show_min_json);
  REQUIRE(rr::render_run_show_json(min_run(), {}) == k_run_show_min_json);
  REQUIRE(rr::render_bench_show_text(min_run(), {}, {}) == k_bench_show_min_text);
  REQUIRE(rr::render_run_show_text(min_run(), {}) == k_run_show_min_text);

  // The unset corpus_repo / ended_at rows are absent from the text form, not
  // rendered blank -- while the empty-but-set base_sha / config_hash rows ARE
  // present and keep their trailing padding.
  const auto text = rr::render_bench_show_text(min_run(), {}, {});
  REQUIRE(text.find("corpus_repo") == std::string::npos);
  REQUIRE(text.find("ended_at") == std::string::npos);
  REQUIRE(text.find("base_sha:    \n") != std::string::npos);
}

TEST_CASE("runs.render: a raw payload is spliced in verbatim, not re-escaped", "[runs]") {
  auto run_ = min_run();
  // Oracle-captured: `--payload '   {"a":1}   '` round-trips with its padding
  // intact, producing `"payload":   {"a":1}   ,` -- proof the blob is spliced
  // rather than serialized. A string-escaping renderer would emit
  // `"payload":"   {\"a\":1}   "` instead.
  const std::vector<rl::event_row> events{
      rl::event_row{.id         = 10,
                    .run_id     = 2,
                    .seq        = 1,
                    .kind       = "padded",
                    .payload    = std::string{R"(   {"a":1}   )"},
                    .created_at = "2020-01-01T00:00:00.000Z"},
  };
  const auto out = rr::render_run_show_json(run_, events);
  REQUIRE(out.find(R"("payload":   {"a":1}   ,)") != std::string::npos);
  REQUIRE(out.find(R"(\")") == std::string::npos);

  // The same holds for config_json on the bench surface.
  auto with_config        = min_run();
  with_config.config_json = std::string{R"({"k":[1,2]})"};
  const auto bench_out    = rr::render_bench_show_json(with_config, {}, {});
  REQUIRE(bench_out.find(R"("config_json":{"k":[1,2]},)") != std::string::npos);
}

TEST_CASE("runs.render: a bare-null payload is indistinguishable from an absent one", "[runs]") {
  // `--payload null` is accepted by the oracle (exit 0) and stored as the
  // four bytes `null`, which render identically to a NULL column. Pinned so
  // the ambiguity is a known, deliberate property rather than a surprise.
  const std::vector<rl::event_row> stored_null{
      rl::event_row{
          .id = 1, .run_id = 2, .seq = 1, .kind = "k", .payload = std::string{"null"}, .created_at = "2020-01-01T00:00:00.000Z"},
  };
  const std::vector<rl::event_row> absent{
      rl::event_row{
          .id = 1, .run_id = 2, .seq = 1, .kind = "k", .payload = std::nullopt, .created_at = "2020-01-01T00:00:00.000Z"},
  };
  REQUIRE(rr::render_run_show_json(min_run(), stored_null) == rr::render_run_show_json(min_run(), absent));
}

TEST_CASE("runs.render: string escaping matches the oracle's captured bytes", "[runs]") {
  rl::run run_{
      .id          = 11,
      .run_uid     = "q\"uid\\back",
      .plan_id     = 1,
      .arm         = "a\"rm\ttab",
      .base_sha    = "sha\nnl",
      .config_hash = "h\xe2\x82\xac\xc3\xbc", // h€ü as raw UTF-8
      .config_json = std::nullopt,
      .corpus_repo = std::string{"c/r"},
      .status      = "running",
      .started_at  = "2020-01-01T00:00:00.000Z",
      .ended_at    = std::nullopt,
  };
  // Three C0 control bytes, deliberately chosen so their hex escapes contain
  // LETTERS. 0x01 alone does not discriminate lowercase from uppercase hex --
  // "0001" has no letter in it -- so a case regression sails past a 0x01-only
  // fixture. (Found by break-probe: the uppercase-hex mutant SURVIVED against
  // the original 0x01-only version of this test.) 0x0b, 0x1b and 0x1f all
  // carry a letter and were captured from the oracle directly:
  //
  //   $Z bench event hexcase --kind $'vt\x0bhere'  --seq 1
  //   $Z bench event hexcase --kind $'us\x1fhere'  --seq 2
  //   $Z bench event hexcase --kind $'esc\x1bhere' --seq 3
  //   $Z bench show hexcase --json
  //     -> "kind":"vt\u000bhere"   "kind":"us\u001fhere"   "kind":"esc\u001bhere"
  const std::vector<rl::event_row> events{
      rl::event_row{.id         = 9,
                    .run_id     = 11,
                    .seq        = 1,
                    .kind       = std::string{"k\x01"
                                              "ctl"},
                    .payload    = std::nullopt,
                    .created_at = "2020-01-01T00:00:00.000Z"},
      rl::event_row{.id         = 10,
                    .run_id     = 11,
                    .seq        = 2,
                    .kind       = std::string{"vt\x0b"
                                              "here"},
                    .payload    = std::nullopt,
                    .created_at = "2020-01-01T00:00:00.000Z"},
      rl::event_row{.id         = 11,
                    .run_id     = 11,
                    .seq        = 3,
                    .kind       = std::string{"us\x1f"
                                              "here"},
                    .payload    = std::nullopt,
                    .created_at = "2020-01-01T00:00:00.000Z"},
      rl::event_row{.id         = 12,
                    .run_id     = 11,
                    .seq        = 4,
                    .kind       = std::string{"esc\x1b"
                                              "here"},
                    .payload    = std::nullopt,
                    .created_at = "2020-01-01T00:00:00.000Z"},
  };
  const auto out = rr::render_bench_show_json(run_, events, {});

  REQUIRE(out.find(R"("run_uid":"q\"uid\\back")") != std::string::npos);
  REQUIRE(out.find(R"("arm":"a\"rm\ttab")") != std::string::npos);
  REQUIRE(out.find(R"("base_sha":"sha\nnl")") != std::string::npos);
  // Non-ASCII passes through as raw UTF-8 -- NOT \u-escaped.
  REQUIRE(out.find("\"config_hash\":\"h\xe2\x82\xac\xc3\xbc\"") != std::string::npos);
  // A bare slash is NOT escaped to \/.
  REQUIRE(out.find(R"("corpus_repo":"c/r")") != std::string::npos);
  // C0 controls escape to \u00xx. NOT spelled as raw string literals: the
  // expectation is the six literal characters backslash-u-0-0-0-1, which a
  // raw literal would leave ambiguous with a universal-character-name.
  REQUIRE(out.find("\"kind\":\"k\\u0001ctl\"") != std::string::npos);
  // ...and the hex digits are LOWERCASE, matching zig's outputUnicodeEscape
  // and diverging from Glaze's own writer, which hardcodes
  // "0123456789ABCDEF" with no option to change it (see cli/output.cppm's
  // note on the same divergence). These three are the assertions that
  // actually discriminate the case -- 0x01's "0001" has no letter to differ
  // in.
  REQUIRE(out.find("\"kind\":\"vt\\u000bhere\"") != std::string::npos);
  REQUIRE(out.find("\"kind\":\"us\\u001fhere\"") != std::string::npos);
  REQUIRE(out.find("\"kind\":\"esc\\u001bhere\"") != std::string::npos);
  REQUIRE(out.find("\\u000B") == std::string::npos);
  REQUIRE(out.find("\\u001F") == std::string::npos);
  REQUIRE(out.find("\\u001B") == std::string::npos);
  // None of the raw control bytes may survive into the output.
  REQUIRE(out.find('\x01') == std::string::npos);
  REQUIRE(out.find('\x0b') == std::string::npos);
  REQUIRE(out.find('\x1b') == std::string::npos);
  REQUIRE(out.find('\x1f') == std::string::npos);
}

TEST_CASE("runs.render: the write leaves' envelopes match the oracle", "[runs]") {
  // `run start` / `run event` / `run finish` emit JSON unconditionally -- the
  // `--json` flag exists but changes nothing (oracle-confirmed).
  REQUIRE(rr::render_run_start_json("c5678087d1831bc7fe1e47a35d35f5f3", 1, "op") ==
          R"({"run_uid":"c5678087d1831bc7fe1e47a35d35f5f3","plan_id":1,"arm":"op"})");
  // --workflow overrides the arm, it does not add a field.
  REQUIRE(rr::render_run_start_json("1dab822ffd79ff9e6463728b3fb6dfbb", 1, "wf1") ==
          R"({"run_uid":"1dab822ffd79ff9e6463728b3fb6dfbb","plan_id":1,"arm":"wf1"})");
  REQUIRE(rr::render_run_event_json("u", 3, "step2") == R"({"run_uid":"u","seq":3,"kind":"step2"})");
  REQUIRE(rr::render_run_finish_json("u", "completed") == R"({"run_uid":"u","status":"completed"})");

  // The bench write leaves are NOT json and have no --json flag at all.
  REQUIRE(rr::render_bench_start("r1") == "r1");
  REQUIRE(rr::render_bench_ok() == "ok");
}

TEST_CASE("runs.render: arms warn but are not refused; statuses are refused", "[runs]") {
  REQUIRE(rr::is_known_arm("strict"));
  REQUIRE(rr::is_known_arm("eligibility"));
  REQUIRE(rr::is_known_arm("grouped"));
  // `op` is what `run start` writes and it is NOT in the recognized set --
  // which is exactly why an unrecognized arm must warn rather than refuse.
  REQUIRE_FALSE(rr::is_known_arm("op"));
  REQUIRE_FALSE(rr::is_known_arm("a1"));
  REQUIRE_FALSE(rr::is_known_arm(""));

  REQUIRE(rr::is_valid_terminal_status("completed"));
  REQUIRE(rr::is_valid_terminal_status("aborted"));
  REQUIRE(rr::is_valid_terminal_status("error"));
  // Oracle-confirmed refusals at exit 2. `ok` and `failed` read like they
  // should work and do not; `running` is the column's own default and is
  // still not an accepted TERMINAL status.
  REQUIRE_FALSE(rr::is_valid_terminal_status("ok"));
  REQUIRE_FALSE(rr::is_valid_terminal_status("failed"));
  REQUIRE_FALSE(rr::is_valid_terminal_status("running"));
  REQUIRE_FALSE(rr::is_valid_terminal_status("Completed"));
}

TEST_CASE("runs.render: JSON payload validation rejects trailing content", "[runs]") {
  REQUIRE(rr::is_valid_json_payload(R"({"a":1})"));
  // Oracle-confirmed accepted: surrounding whitespace, and any bare JSON
  // value -- not just objects.
  REQUIRE(rr::is_valid_json_payload(R"(   {"a":1}   )"));
  REQUIRE(rr::is_valid_json_payload("123"));
  REQUIRE(rr::is_valid_json_payload("null"));
  REQUIRE(rr::is_valid_json_payload(R"("str")"));

  // Oracle-confirmed refused at exit 2. The trailing-content case is the one
  // a plain glz::read_json would silently accept (task 6086).
  REQUIRE_FALSE(rr::is_valid_json_payload(R"({"a":1} junk)"));
  REQUIRE_FALSE(rr::is_valid_json_payload("notjson"));
  REQUIRE_FALSE(rr::is_valid_json_payload(""));
}

TEST_CASE("runs.render: the diagnostic strings match the oracle verbatim", "[runs]") {
  REQUIRE(rr::render_unknown_arm_warning("a1") ==
          "warn: bench start: unrecognized arm 'a1'; recognized arms: strict, eligibility, grouped");

  // One wording, six leaves -- the prefix is the only difference.
  REQUIRE(rr::render_run_not_found("bench show", "nope") == "bench show: run 'nope' not found");
  REQUIRE(rr::render_run_not_found("bench event", "nope") == "bench event: run 'nope' not found");
  REQUIRE(rr::render_run_not_found("bench touch", "nope") == "bench touch: run 'nope' not found");
  REQUIRE(rr::render_run_not_found("bench finish", "nope") == "bench finish: run 'nope' not found");
  REQUIRE(rr::render_run_not_found("run show", "nope") == "run show: run 'nope' not found");
  REQUIRE(rr::render_run_not_found("run finish", "nope") == "run finish: run 'nope' not found");

  REQUIRE(rr::render_invalid_status("bench finish", "ok") ==
          "bench finish: invalid --status 'ok'; expected completed, aborted, or error");
  REQUIRE(rr::render_invalid_status("run finish", "bogus") ==
          "run finish: invalid --status 'bogus'; expected completed, aborted, or error");
  REQUIRE(rr::render_invalid_touch_kind("bogus") == "bench touch: invalid --kind 'bogus'; expected declared or actual");
  // The rejected blob is echoed in full, and the flag name is part of the
  // message -- both captured.
  REQUIRE(rr::render_invalid_json("bench event", "--payload", "notjson") == "bench event: --payload is not valid JSON: notjson");
  REQUIRE(rr::render_invalid_json("run event", "--payload", "nope") == "run event: --payload is not valid JSON: nope");
  REQUIRE(rr::render_invalid_json("bench start", "--config-json", "notjson") ==
          "bench start: --config-json is not valid JSON: notjson");
  REQUIRE(rr::render_duplicate_run_uid("r1") == "bench start: run_uid 'r1' already exists");
  REQUIRE(rr::render_duplicate_seq(1, "r1") == "bench event: seq 1 already used for run 'r1'");
}
