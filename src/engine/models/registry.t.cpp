// @file registry.t.cpp
// @brief Unit tests for `planar.engine.models.registry` (plan 996, task 6096).
//
// ORACLE PROVENANCE. Every shape, ordering, gate verdict, and error outcome
// below was captured by RUNNING the Zig binary against a scratch database --
// never from `--help`, never inferred from the Zig source. Every probe ran
// with HOME, PLANAR_HOME and CODEX_HOME redirected under /tmp so no probe
// could touch the developer's real `~/.planar`:
//
//   export ORACLE=./zig/zig-out/bin/planar
//   export PLANAR_DB=/tmp/op/probe.db PLANAR_CONFIG_PATH=/tmp/op/probe.toml
//   export HOME=/tmp/op/home PLANAR_HOME=/tmp/op/home/.planar
//   export CODEX_HOME=/tmp/op/home/.codex
//   p(){ "$ORACLE" "$@"; }        # a FUNCTION, not `$B $c`: zsh does not
//                                 # word-split an unquoted $var, and the
//                                 # naive form reports UnknownSubcommand for
//                                 # every multi-word verb.
//   cd /tmp/op/work && p init
//
// --- add / duplicate ------------------------------------------------------
//   p models registry add --vendor anthropic --id claude-opus-5 --order 1
//       -> exit 0, stdout "1"
//   p models registry add --vendor anthropic --id claude-sonnet-5 --order 2 --disabled
//       -> exit 0, stdout "2"
//   p models registry add --vendor anthropic --id claude-opus-5 --order 3
//       -> exit 3, stderr "error: registering opaque candidate: Conflict"
//
// --- list ordering and text shape -----------------------------------------
//   p models registry list
//       1 anthropic claude-opus-5 enabled=true order=1 bindings=1 observation=present
//       2 anthropic claude-sonnet-5 enabled=false order=2 bindings=0 observation=none
//
// --- bind: idempotent; missing candidate is QueryFailed, NOT NotFound ------
//   p models registry bind --candidate 1 --role coder --tier medium  -> exit 0, empty
//   p models registry bind --candidate 1 --role coder --tier medium  -> exit 0, empty
//       (list still reports bindings=1)
//   p models registry bind --candidate 99 --role coder --tier medium
//       -> exit 1, stderr "error: binding candidate: QueryFailed"
//
// --- update rewrites `enabled` unconditionally ----------------------------
//   candidate 2 was added --disabled; then:
//   p models registry update --candidate 2 --order 7  -> exit 0, empty
//   p models registry list
//       2 anthropic claude-sonnet-5 enabled=true order=7 ...   <-- RE-ENABLED
//
// --- remove ---------------------------------------------------------------
//   p models registry remove --candidate 2   -> exit 0, empty
//   p models registry remove --candidate 99  -> exit 1
//       stderr "error: removing candidate: NotFound"
//
// --- observe / eligibility ------------------------------------------------
//   p models registry observe --candidate 1 --host claude-code --version 1 \
//       --availability available --spawn-verification verified \
//       --evidence-ref ev-1 --captured-at 2026-08-01T00:00:00Z \
//       --expires-at 2026-09-01T00:00:00Z            -> exit 0, stdout "1"
//
//   p models registry eligibility --candidate 1 --host claude-code \
//       --role coder --tier medium --now 2026-08-15T00:00:00Z
//     {"candidate":1,"host":"claude-code","eligible":false,
//      "gates":{"cli_available":true,"exact_spawn_verified":true,
//               "role_tier_bound":true,"role_surface_override_supported":false,
//               "host_policy_permits":false,"observation_fresh":true},
//      "reasons":["role_surface_override_unsupported","host_policy_denied"]}
//
//   ... --now 2026-10-15T00:00:00Z   -> observation_fresh:false, reasons gains
//                                       "host_observation_expired"
//   ... --override-supported --policy-permits  -> "eligible":true,"reasons":[]
//   ... --host other-host --override-supported --policy-permits
//       -> cli_available:false, exact_spawn_verified:false,
//          observation_fresh:false   [another host's observation is NEVER
//                                     substituted]
//   ... --role reviewer --override-supported --policy-permits
//       -> role_tier_bound:false, reasons ["role_tier_not_bound"]
//   candidate 2 (disabled, no binding, no observation), all flags off:
//       -> all six gates false, all six reasons named, in gate order
//   p models registry eligibility --candidate 99 ...
//       -> exit 1, stderr "error: reading candidate: NotFound"
//   p models registry eligibility ... --json
//       -> exit 2, stderr "error: unknown flag (got --json)"   [NO --json flag]
//
// --- verify-identity ------------------------------------------------------
//   p models registry verify-identity --candidate 1 \
//       --actual-vendor anthropic --actual-id claude-opus-5
//       -> {"candidate":1,"identity":"matched"}
//   ... --actual-id claude-opus-4      -> {"candidate":1,"identity":"candidate_mismatch"}
//   ... --actual-vendor openai --actual-id cand-1
//                                      -> {"candidate":1,"identity":"vendor_mismatch"}
//
// --- EMPTY DATABASE (hazard 6) --------------------------------------------
//   p models registry list --json
//     {"registry_version":1,"candidates":[],"migration_warning":"legacy catalog
//      compatibility is one-window and non-authoritative"}
//   p models registry list        -> exit 0, EMPTY stdout (no "none" line)
//   p models registry export --json  -> identical envelope to list --json
//
// `models resolve` is NOT covered: it is not ported (see CMakeLists.txt's cut
// list -- it rests on the roles/profile/packet subsystem). Its refusal
// surface is recorded there for the cycle that takes it.

#include <catch2/catch_test_macros.hpp>

import std;
import planar.db;
import planar.db.migrate;
import planar.engine.models.registry;

namespace {

namespace reg = planar::engine::models::registry;

struct scratch_db_path {
  std::filesystem::path path_;

  scratch_db_path()
      : path_(std::filesystem::temp_directory_path() / std::format("planar_models_registry_test_{}_{}.db",
                                                                   std::chrono::steady_clock::now().time_since_epoch().count(),
                                                                   reinterpret_cast<std::uintptr_t>(this))) {
  }

  scratch_db_path(const scratch_db_path&)            = delete;
  scratch_db_path& operator=(const scratch_db_path&) = delete;

  ~scratch_db_path() {
    std::error_code ec;
    std::filesystem::remove(path_, ec);
    std::filesystem::remove(path_.string() + "-journal", ec);
    std::filesystem::remove(path_.string() + "-wal", ec);
    std::filesystem::remove(path_.string() + "-shm", ec);
  }
};

auto open_migrated(const scratch_db_path& scratch) -> planar::db::connection {
  auto conn = planar::db::connection::open(scratch.path_.string());
  REQUIRE(conn.has_value());
  auto applied = planar::db::apply_all(*conn);
  REQUIRE(applied.has_value());
  return std::move(*conn);
}

/// @brief The observation the oracle transcript recorded on candidate 1.
auto fresh_observation(std::int64_t candidate_id) -> reg::observe_args {
  return reg::observe_args{
      .candidate_id        = candidate_id,
      .host_id             = "claude-code",
      .observation_version = 1,
      .availability_       = reg::availability::available,
      .spawn_verification_ = reg::spawn_verification::verified,
      .evidence_ref        = "ev-1",
      .captured_at         = "2026-08-01T00:00:00Z",
      .expires_at          = "2026-09-01T00:00:00Z",
  };
}

} // namespace

TEST_CASE("models.registry: the three enum sets round-trip their schema text", "[models]") {
  REQUIRE(reg::tier_from_text("small") == reg::tier::small);
  REQUIRE(reg::tier_from_text("medium") == reg::tier::medium);
  REQUIRE(reg::tier_from_text("large") == reg::tier::large);
  REQUIRE_FALSE(reg::tier_from_text("Medium").has_value()); // the CHECK is case-sensitive
  REQUIRE_FALSE(reg::tier_from_text("").has_value());
  REQUIRE(reg::tier_to_text(reg::tier::small) == "small");
  REQUIRE(reg::tier_to_text(reg::tier::medium) == "medium");
  REQUIRE(reg::tier_to_text(reg::tier::large) == "large");

  // THREE availability values, not two. `unknown` ("we did not find out") is
  // a different claim from `unavailable` ("we checked and it is down"), and
  // the schema CHECK carries all three. Oracle-confirmed the CLI stores an
  // `unknown` row and rejects only genuinely unknown spellings:
  //   $Z models registry observe ... --availability unknown  -> exit 0
  //   $Z models registry observe ... --availability bogus
  //       -> "error: invalid availability: bogus"
  REQUIRE(reg::availability_from_text("available") == reg::availability::available);
  REQUIRE(reg::availability_from_text("unavailable") == reg::availability::unavailable);
  REQUIRE(reg::availability_from_text("unknown") == reg::availability::unknown);
  REQUIRE_FALSE(reg::availability_from_text("Available").has_value());
  REQUIRE_FALSE(reg::availability_from_text("bogus").has_value());
  REQUIRE(reg::availability_to_text(reg::availability::available) == "available");
  REQUIRE(reg::availability_to_text(reg::availability::unavailable) == "unavailable");
  REQUIRE(reg::availability_to_text(reg::availability::unknown) == "unknown");

  // FOUR spawn-verification values. `mismatch` in particular -- the spawn
  // completed and returned a DIFFERENT identity -- is the alarm condition the
  // whole opaque-identity discipline exists to surface, and collapsing it into
  // `unverified` would erase it. Oracle-confirmed all four are stored:
  //   sqlite3 p.db 'select host_id,availability,spawn_verification
  //                 from routing_host_observations order by id'
  //     ha1|available|verified     hs5|available|unverified
  //     ha2|unavailable|verified   hs6|available|failed
  //     ha3|unknown|verified       hs7|available|mismatch
  REQUIRE(reg::spawn_verification_from_text("verified") == reg::spawn_verification::verified);
  REQUIRE(reg::spawn_verification_from_text("unverified") == reg::spawn_verification::unverified);
  REQUIRE(reg::spawn_verification_from_text("failed") == reg::spawn_verification::failed);
  REQUIRE(reg::spawn_verification_from_text("mismatch") == reg::spawn_verification::mismatch);
  REQUIRE_FALSE(reg::spawn_verification_from_text("VERIFIED").has_value());
  REQUIRE_FALSE(reg::spawn_verification_from_text("bogus").has_value());
  REQUIRE(reg::spawn_verification_to_text(reg::spawn_verification::verified) == "verified");
  REQUIRE(reg::spawn_verification_to_text(reg::spawn_verification::unverified) == "unverified");
  REQUIRE(reg::spawn_verification_to_text(reg::spawn_verification::failed) == "failed");
  REQUIRE(reg::spawn_verification_to_text(reg::spawn_verification::mismatch) == "mismatch");
}

TEST_CASE("models.registry: the widened enum values round-trip through SQLite", "[models]") {
  // Not just parse/render: a stored `unknown` / `failed` / `mismatch` row must
  // read back as itself. Before these values were added to the enums,
  // `read_observation` refused such a row as `invalid_value` -- a registry
  // populated by the real CLI would have been UNREADABLE by this port.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(reg::create(conn, {.vendor = "v", .candidate_id = "c", .fallback_order = 1}).has_value());

  auto unknown          = fresh_observation(1);
  unknown.host_id       = "ha3";
  unknown.availability_ = reg::availability::unknown;
  REQUIRE(reg::observe(conn, unknown).has_value());

  auto mismatched                = fresh_observation(1);
  mismatched.host_id             = "hs7";
  mismatched.spawn_verification_ = reg::spawn_verification::mismatch;
  REQUIRE(reg::observe(conn, mismatched).has_value());

  auto failed                = fresh_observation(1);
  failed.host_id             = "hs6";
  failed.spawn_verification_ = reg::spawn_verification::failed;
  REQUIRE(reg::observe(conn, failed).has_value());

  auto ha3 = reg::get_for_host(conn, 1, "ha3");
  REQUIRE(ha3.has_value());
  REQUIRE(ha3->latest_observation->availability_ == reg::availability::unknown);
  REQUIRE(ha3->latest_observation->spawn_verification_ == reg::spawn_verification::verified);

  auto hs7 = reg::get_for_host(conn, 1, "hs7");
  REQUIRE(hs7.has_value());
  REQUIRE(hs7->latest_observation->spawn_verification_ == reg::spawn_verification::mismatch);

  auto hs6 = reg::get_for_host(conn, 1, "hs6");
  REQUIRE(hs6.has_value());
  REQUIRE(hs6->latest_observation->spawn_verification_ == reg::spawn_verification::failed);

  // The gate semantics of the widened values, oracle-captured verbatim:
  //
  //   host ha3 (availability=unknown, spawn=verified):
  //     "cli_available":false,"exact_spawn_verified":true,
  //     "reasons":["provider_cli_unavailable"]
  //   host hs7 (availability=available, spawn=mismatch):
  //     "cli_available":true,"exact_spawn_verified":false,
  //     "reasons":["exact_spawn_unverified"]
  //   host hs6 (availability=available, spawn=failed): same as hs7
  //
  // `unknown` fails cli_available (only `available` passes); `failed` and
  // `mismatch` both fail exact_spawn_verified (only `verified` passes).
  auto gates_for = [&](const reg::candidate& value) {
    return reg::evaluate_eligibility({.enabled                         = true,
                                      .binding_present                 = true,
                                      .role_surface_override_supported = true,
                                      .host_policy_permits             = true,
                                      .observation                     = value.latest_observation,
                                      .now                             = "2026-08-15T00:00:00Z"});
  };
  const auto unknown_gates = gates_for(*ha3);
  REQUIRE_FALSE(unknown_gates.cli_available);
  REQUIRE(unknown_gates.exact_spawn_verified);
  REQUIRE(reg::reasons(unknown_gates) == std::vector<reg::eligibility_reason>{reg::eligibility_reason::provider_cli_unavailable});

  for (const auto* value : {&*hs7, &*hs6}) {
    const auto gates = gates_for(*value);
    REQUIRE(gates.cli_available);
    REQUIRE_FALSE(gates.exact_spawn_verified);
    REQUIRE(reg::reasons(gates) == std::vector<reg::eligibility_reason>{reg::eligibility_reason::exact_spawn_unverified});
  }
}

TEST_CASE("models.registry: an empty timestamp binds as text, not as SQL NULL", "[models]") {
  // A BREAK-PROBE SURVIVOR fixed. This case was written when the guard was a
  // local `nn()` in registry.cpp; task 6097 has since moved the fix to its
  // root in `db::statement::bind_text` and deleted all thirteen local
  // guards, so this test now pins the ROOT behaviour through this module's
  // reachable path. It still discriminates: reverting `bind_text` to the raw
  // `value.data()` bind fails it. Every other test in this file passes real
  // non-empty strings, which is why it survived before.
  //
  // `captured_at` is the reachable case: `routing_host_observations` declares
  // it `text not null` with NO length CHECK, and the only validation this
  // module applies is the ordering test `captured_at < expires_at`, which an
  // empty string PASSES. So a default-constructed view reaches the bind, and
  // without the guard it violates NOT NULL and surfaces as `conflict`.
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(reg::create(conn, {.vendor = "v", .candidate_id = "c", .fallback_order = 1}).has_value());

  auto blank        = fresh_observation(1);
  blank.captured_at = std::string_view{}; // null data(), zero size
  auto written      = reg::observe(conn, blank);
  REQUIRE(written.has_value());

  auto readback = reg::get_for_host(conn, 1, "claude-code");
  REQUIRE(readback.has_value());
  REQUIRE(readback->latest_observation.has_value());
  // Stored as the EMPTY STRING, not as NULL.
  REQUIRE(readback->latest_observation->captured_at.empty());
}

TEST_CASE("models.registry: opaque values keep punctuation but refuse control bytes", "[models]") {
  // The whole point of an OPAQUE identifier is that Planar does not get an
  // opinion about its text. Shell metacharacters, spaces, and leading dashes
  // all survive; only the empty string and C0/DEL are refused.
  REQUIRE(reg::valid_opaque_value("claude-opus-5"));
  REQUIRE(reg::valid_opaque_value("opaque -- value; $()"));
  REQUIRE(reg::valid_opaque_value("--looks-like-a-flag"));
  REQUIRE(reg::valid_opaque_value("h€ü")); // non-ASCII passes through
  REQUIRE_FALSE(reg::valid_opaque_value(""));
  REQUIRE_FALSE(reg::valid_opaque_value(std::string_view{"tab\there", 8}));
  REQUIRE_FALSE(reg::valid_opaque_value(std::string_view{"nl\nhere", 7}));
  REQUIRE_FALSE(reg::valid_opaque_value(std::string_view{"ctl\x01here", 8}));
  REQUIRE_FALSE(reg::valid_opaque_value(std::string_view{"del\x7fhere", 8}));
  // 0x20 (space) is the first ACCEPTED byte -- the boundary itself, not just
  // a value near it.
  REQUIRE(reg::valid_opaque_value(std::string_view{"a b", 3}));
  REQUIRE_FALSE(reg::valid_opaque_value(std::string_view{"a\x1f"
                                                         "b",
                                                         3}));
}

TEST_CASE("models.registry: add mints ids, refuses a duplicate opaque identity", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto first = reg::create(conn, {.vendor = "anthropic", .candidate_id = "claude-opus-5", .fallback_order = 1});
  REQUIRE(first.has_value());
  REQUIRE(*first == 1);

  auto second =
      reg::create(conn, {.vendor = "anthropic", .candidate_id = "claude-sonnet-5", .enabled = false, .fallback_order = 2});
  REQUIRE(second.has_value());
  REQUIRE(*second == 2);

  // Same (vendor, candidate_id) -- Conflict, mapped from the UNIQUE index.
  auto dup = reg::create(conn, {.vendor = "anthropic", .candidate_id = "claude-opus-5", .fallback_order = 3});
  REQUIRE_FALSE(dup.has_value());
  REQUIRE(dup.error() == reg::registry_error::conflict);

  // The SAME candidate id under a DIFFERENT vendor is a different candidate.
  auto other_vendor = reg::create(conn, {.vendor = "openai", .candidate_id = "claude-opus-5", .fallback_order = 4});
  REQUIRE(other_vendor.has_value());
}

TEST_CASE("models.registry: add refuses invalid values before reaching SQLite", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto empty_vendor = reg::create(conn, {.vendor = "", .candidate_id = "c", .fallback_order = 0});
  REQUIRE_FALSE(empty_vendor.has_value());
  REQUIRE(empty_vendor.error() == reg::registry_error::invalid_value);

  auto empty_id = reg::create(conn, {.vendor = "v", .candidate_id = "", .fallback_order = 0});
  REQUIRE_FALSE(empty_id.has_value());
  REQUIRE(empty_id.error() == reg::registry_error::invalid_value);

  auto negative_order = reg::create(conn, {.vendor = "v", .candidate_id = "c", .fallback_order = -1});
  REQUIRE_FALSE(negative_order.has_value());
  REQUIRE(negative_order.error() == reg::registry_error::invalid_value);

  auto bad_source =
      reg::create(conn, {.vendor = "v", .candidate_id = "c", .fallback_order = 0, .compatibility_source = "invented"});
  REQUIRE_FALSE(bad_source.has_value());
  REQUIRE(bad_source.error() == reg::registry_error::invalid_value);

  // Both sanctioned sources ARE accepted -- the check is a closed set, not a
  // single literal.
  REQUIRE(reg::create(conn, {.vendor = "v", .candidate_id = "c1", .fallback_order = 0, .compatibility_source = "native"})
              .has_value());
  REQUIRE(reg::create(conn, {.vendor = "v", .candidate_id = "c2", .fallback_order = 0, .compatibility_source = "legacy_config"})
              .has_value());

  // Order 0 is legal; only NEGATIVE is refused.
  REQUIRE(reg::create(conn, {.vendor = "v", .candidate_id = "c3", .fallback_order = 0}).has_value());
}

TEST_CASE("models.registry: list orders by vendor, then fallback order, then id", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  // Inserted deliberately OUT of the expected read order.
  REQUIRE(reg::create(conn, {.vendor = "openai", .candidate_id = "gpt-b", .fallback_order = 1}).has_value());
  REQUIRE(reg::create(conn, {.vendor = "anthropic", .candidate_id = "zed", .fallback_order = 9}).has_value());
  REQUIRE(reg::create(conn, {.vendor = "anthropic", .candidate_id = "beta", .fallback_order = 2}).has_value());
  REQUIRE(reg::create(conn, {.vendor = "anthropic", .candidate_id = "alpha", .fallback_order = 2}).has_value());

  auto listed = reg::list(conn);
  REQUIRE(listed.has_value());
  REQUIRE(listed->size() == 4);
  // vendor asc, then fallback_order asc, then candidate_id asc -- NOT
  // insertion order and NOT id order.
  REQUIRE((*listed)[0].registration_.candidate_id == "alpha");
  REQUIRE((*listed)[1].registration_.candidate_id == "beta");
  REQUIRE((*listed)[2].registration_.candidate_id == "zed");
  REQUIRE((*listed)[3].registration_.vendor == "openai");
}

TEST_CASE("models.registry: an empty registry lists as an empty vector, not an error", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);

  auto listed = reg::list(conn);
  REQUIRE(listed.has_value());
  REQUIRE(listed->empty());
}

TEST_CASE("models.registry: find_id resolves an opaque pair, or reports absence", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(reg::create(conn, {.vendor = "anthropic", .candidate_id = "claude-opus-5", .fallback_order = 1}).has_value());

  auto found = reg::find_id(conn, "anthropic", "claude-opus-5");
  REQUIRE(found.has_value());
  REQUIRE(*found == 1);

  auto missing = reg::find_id(conn, "anthropic", "nope");
  REQUIRE(missing.has_value());
  REQUIRE_FALSE(missing->has_value());

  // Vendor participates in the lookup -- the same id under another vendor is
  // a miss, not a hit.
  auto wrong_vendor = reg::find_id(conn, "openai", "claude-opus-5");
  REQUIRE(wrong_vendor.has_value());
  REQUIRE_FALSE(wrong_vendor->has_value());
}

TEST_CASE("models.registry: update rewrites enabled unconditionally and bumps the version", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(reg::create(conn, {.vendor = "v", .candidate_id = "c", .enabled = false, .fallback_order = 2}).has_value());

  auto before = reg::list(conn);
  REQUIRE(before.has_value());
  REQUIRE_FALSE((*before)[0].registration_.enabled);
  REQUIRE((*before)[0].registration_.registration_version == 1);

  // `models registry update` has no --enabled flag: it passes `!--disabled`,
  // so reordering a DISABLED candidate silently re-enables it. Oracle-captured.
  REQUIRE(reg::update(conn, 1, true, 7).has_value());

  auto after = reg::list(conn);
  REQUIRE(after.has_value());
  REQUIRE((*after)[0].registration_.enabled);
  REQUIRE((*after)[0].registration_.fallback_order == 7);
  REQUIRE((*after)[0].registration_.registration_version == 2);

  // A missing row is not_found, NOT a silent success.
  auto missing = reg::update(conn, 99, true, 1);
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error() == reg::registry_error::not_found);

  auto negative = reg::update(conn, 1, true, -1);
  REQUIRE_FALSE(negative.has_value());
  REQUIRE(negative.error() == reg::registry_error::invalid_value);
}

TEST_CASE("models.registry: remove reports not_found, and is refused by recorded evidence", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(reg::create(conn, {.vendor = "v", .candidate_id = "c", .fallback_order = 1}).has_value());

  auto missing = reg::remove(conn, 99);
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error() == reg::registry_error::not_found);

  // An observation is immutable evidence: `routing_host_observations` holds an
  // `on delete restrict` FK, so the candidate can no longer be deleted. This
  // is what keeps the audit trail from being orphaned.
  REQUIRE(reg::observe(conn, fresh_observation(1)).has_value());
  auto restricted = reg::remove(conn, 1);
  REQUIRE_FALSE(restricted.has_value());
  REQUIRE(restricted.error() == reg::registry_error::query_failed);

  // With no evidence pointing at it, removal succeeds.
  REQUIRE(reg::create(conn, {.vendor = "v", .candidate_id = "c2", .fallback_order = 2}).has_value());
  REQUIRE(reg::remove(conn, 2).has_value());
  auto listed = reg::list(conn);
  REQUIRE(listed.has_value());
  REQUIRE(listed->size() == 1);
}

TEST_CASE("models.registry: bind is idempotent, unbind is not existence-checked", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(reg::create(conn, {.vendor = "v", .candidate_id = "c", .fallback_order = 1}).has_value());

  REQUIRE(reg::bind(conn, 1, "coder", reg::tier::medium).has_value());
  REQUIRE(reg::bind(conn, 1, "coder", reg::tier::medium).has_value()); // repeat: still ok

  auto listed = reg::list(conn);
  REQUIRE(listed.has_value());
  REQUIRE((*listed)[0].bindings.size() == 1); // the repeat wrote NOTHING
  REQUIRE((*listed)[0].bindings[0].role == "coder");
  REQUIRE((*listed)[0].bindings[0].tier_ == reg::tier::medium);
  REQUIRE((*listed)[0].bindings[0].candidate_id == 1);

  // A different tier for the same role is a SEPARATE binding.
  REQUIRE(reg::bind(conn, 1, "coder", reg::tier::large).has_value());
  auto two = reg::list(conn);
  REQUIRE(two.has_value());
  REQUIRE((*two)[0].bindings.size() == 2);
  // Ordered role, tier -- 'large' sorts before 'medium' as TEXT.
  REQUIRE((*two)[0].bindings[0].tier_ == reg::tier::large);
  REQUIRE((*two)[0].bindings[1].tier_ == reg::tier::medium);

  // Unbinding something never bound is a success, not an error.
  REQUIRE(reg::unbind(conn, 1, "reviewer", reg::tier::small).has_value());
  REQUIRE(reg::unbind(conn, 1, "coder", reg::tier::large).has_value());
  auto one = reg::list(conn);
  REQUIRE(one.has_value());
  REQUIRE((*one)[0].bindings.size() == 1);

  // A missing candidate trips the FK: query_failed, NOT not_found. The oracle
  // renders this as `error: binding candidate: QueryFailed`.
  auto orphan = reg::bind(conn, 99, "coder", reg::tier::medium);
  REQUIRE_FALSE(orphan.has_value());
  REQUIRE(orphan.error() == reg::registry_error::query_failed);

  auto empty_role = reg::bind(conn, 1, "", reg::tier::medium);
  REQUIRE_FALSE(empty_role.has_value());
  REQUIRE(empty_role.error() == reg::registry_error::invalid_value);
}

TEST_CASE("models.registry: observe refuses a window that closes before it opens", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(reg::create(conn, {.vendor = "v", .candidate_id = "c", .fallback_order = 1}).has_value());

  auto ok = reg::observe(conn, fresh_observation(1));
  REQUIRE(ok.has_value());
  REQUIRE(*ok == 1);

  auto backwards                = fresh_observation(1);
  backwards.captured_at         = "2026-09-01T00:00:00Z";
  backwards.expires_at          = "2026-08-01T00:00:00Z";
  backwards.observation_version = 2;
  auto refused                  = reg::observe(conn, backwards);
  REQUIRE_FALSE(refused.has_value());
  REQUIRE(refused.error() == reg::registry_error::invalid_value);

  // EQUAL timestamps are refused too: the comparison is strictly-greater, so
  // an observation cannot be born already expired.
  auto degenerate                = fresh_observation(1);
  degenerate.expires_at          = degenerate.captured_at;
  degenerate.observation_version = 3;
  auto degenerate_refused        = reg::observe(conn, degenerate);
  REQUIRE_FALSE(degenerate_refused.has_value());
  REQUIRE(degenerate_refused.error() == reg::registry_error::invalid_value);

  auto zero_version                = fresh_observation(1);
  zero_version.observation_version = 0;
  auto zero_refused                = reg::observe(conn, zero_version);
  REQUIRE_FALSE(zero_refused.has_value());
  REQUIRE(zero_refused.error() == reg::registry_error::invalid_value);

  auto empty_host    = fresh_observation(1);
  empty_host.host_id = "";
  REQUIRE_FALSE(reg::observe(conn, empty_host).has_value());

  auto empty_evidence         = fresh_observation(1);
  empty_evidence.evidence_ref = "";
  REQUIRE_FALSE(reg::observe(conn, empty_evidence).has_value());
}

TEST_CASE("models.registry: the newest observation wins by version, then by id", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(reg::create(conn, {.vendor = "v", .candidate_id = "c", .fallback_order = 1}).has_value());

  auto v1         = fresh_observation(1);
  v1.evidence_ref = "ev-old";
  REQUIRE(reg::observe(conn, v1).has_value());

  auto v3                = fresh_observation(1);
  v3.observation_version = 3;
  v3.evidence_ref        = "ev-new";
  REQUIRE(reg::observe(conn, v3).has_value());

  // Inserted LAST but with a LOWER version -- must not win.
  auto v2                = fresh_observation(1);
  v2.observation_version = 2;
  v2.evidence_ref        = "ev-mid";
  REQUIRE(reg::observe(conn, v2).has_value());

  auto listed = reg::list(conn);
  REQUIRE(listed.has_value());
  REQUIRE((*listed)[0].latest_observation.has_value());
  REQUIRE((*listed)[0].latest_observation->evidence_ref == "ev-new");
  REQUIRE((*listed)[0].latest_observation->observation_version == 3);
  REQUIRE((*listed)[0].latest_observation->candidate_id == 1);
}

TEST_CASE("models.registry: get_for_host never substitutes another host's observation", "[models]") {
  scratch_db_path scratch;
  auto            conn = open_migrated(scratch);
  REQUIRE(reg::create(conn, {.vendor = "v", .candidate_id = "c", .fallback_order = 1}).has_value());

  auto mine = fresh_observation(1);
  REQUIRE(reg::observe(conn, mine).has_value());

  // Another host, with a much HIGHER version -- the tempting thing to
  // substitute, and exactly what must not happen.
  auto theirs                = fresh_observation(1);
  theirs.host_id             = "other-host";
  theirs.observation_version = 99;
  theirs.evidence_ref        = "ev-theirs";
  REQUIRE(reg::observe(conn, theirs).has_value());

  auto scoped = reg::get_for_host(conn, 1, "claude-code");
  REQUIRE(scoped.has_value());
  REQUIRE(scoped->latest_observation.has_value());
  REQUIRE(scoped->latest_observation->evidence_ref == "ev-1");
  REQUIRE(scoped->latest_observation->host_id == "claude-code");

  // A host with no observation at all sees NONE, not the other host's.
  auto stranger = reg::get_for_host(conn, 1, "third-host");
  REQUIRE(stranger.has_value());
  REQUIRE_FALSE(stranger->latest_observation.has_value());

  auto missing = reg::get_for_host(conn, 99, "claude-code");
  REQUIRE_FALSE(missing.has_value());
  REQUIRE(missing.error() == reg::registry_error::not_found);

  auto bad_host = reg::get_for_host(conn, 1, "");
  REQUIRE_FALSE(bad_host.has_value());
  REQUIRE(bad_host.error() == reg::registry_error::invalid_value);

  // `list`, by contrast, reports the newest across ALL hosts.
  auto listed = reg::list(conn);
  REQUIRE(listed.has_value());
  REQUIRE((*listed)[0].latest_observation->evidence_ref == "ev-theirs");
}

TEST_CASE("models.registry: the six eligibility gates are independent", "[models]") {
  reg::host_observation good{
      .id                  = 1,
      .candidate_id        = 1,
      .host_id             = "claude-code",
      .observation_version = 1,
      .availability_       = reg::availability::available,
      .spawn_verification_ = reg::spawn_verification::verified,
      .evidence_ref        = "ev-1",
      .captured_at         = "2026-08-01T00:00:00Z",
      .expires_at          = "2026-09-01T00:00:00Z",
  };

  const reg::eligibility_input all_good{
      .enabled                         = true,
      .binding_present                 = true,
      .role_surface_override_supported = true,
      .host_policy_permits             = true,
      .observation                     = good,
      .now                             = "2026-08-15T00:00:00Z",
  };
  const auto eligible = reg::evaluate_eligibility(all_good);
  REQUIRE(eligible.eligible());
  REQUIRE(reg::reasons(eligible).empty());

  // Turning off exactly one input turns off exactly one gate.
  auto no_override                            = all_good;
  no_override.role_surface_override_supported = false;
  const auto override_gates                   = reg::evaluate_eligibility(no_override);
  REQUIRE_FALSE(override_gates.eligible());
  REQUIRE(override_gates.cli_available);
  REQUIRE(override_gates.exact_spawn_verified);
  REQUIRE(override_gates.role_tier_bound);
  REQUIRE(override_gates.host_policy_permits);
  REQUIRE(override_gates.observation_fresh);
  REQUIRE(reg::reasons(override_gates) ==
          std::vector<reg::eligibility_reason>{reg::eligibility_reason::role_surface_override_unsupported});

  auto no_policy                = all_good;
  no_policy.host_policy_permits = false;
  REQUIRE(reg::reasons(reg::evaluate_eligibility(no_policy)) ==
          std::vector<reg::eligibility_reason>{reg::eligibility_reason::host_policy_denied});

  // `role_tier_bound` folds `enabled` in: disabling revokes every binding at
  // once without deleting a row.
  auto disabled    = all_good;
  disabled.enabled = false;
  REQUIRE_FALSE(reg::evaluate_eligibility(disabled).role_tier_bound);
  auto unbound            = all_good;
  unbound.binding_present = false;
  REQUIRE_FALSE(reg::evaluate_eligibility(unbound).role_tier_bound);

  auto unavailable              = all_good;
  auto unavailable_obs          = good;
  unavailable_obs.availability_ = reg::availability::unavailable;
  unavailable.observation       = unavailable_obs;
  const auto unavailable_gates  = reg::evaluate_eligibility(unavailable);
  REQUIRE_FALSE(unavailable_gates.cli_available);
  REQUIRE(unavailable_gates.exact_spawn_verified); // INDEPENDENT of availability

  auto unverified                    = all_good;
  auto unverified_obs                = good;
  unverified_obs.spawn_verification_ = reg::spawn_verification::unverified;
  unverified.observation             = unverified_obs;
  const auto unverified_gates        = reg::evaluate_eligibility(unverified);
  REQUIRE_FALSE(unverified_gates.exact_spawn_verified);
  REQUIRE(unverified_gates.cli_available); // INDEPENDENT of verification
}

TEST_CASE("models.registry: no observation fails all three observation-derived gates", "[models]") {
  const reg::eligibility_input none{
      .enabled                         = true,
      .binding_present                 = true,
      .role_surface_override_supported = false,
      .host_policy_permits             = false,
      .observation                     = std::nullopt,
      .now                             = "2026-08-15T00:00:00Z",
  };
  const auto gates = reg::evaluate_eligibility(none);
  REQUIRE_FALSE(gates.cli_available);
  REQUIRE_FALSE(gates.exact_spawn_verified);
  REQUIRE_FALSE(gates.observation_fresh);
  REQUIRE(gates.role_tier_bound); // binding state does NOT depend on evidence

  // All six failing gates, in gate declaration order -- exactly the sequence
  // the oracle emitted for candidate 2 with every flag off.
  const reg::eligibility_input nothing{.enabled                         = false,
                                       .binding_present                 = false,
                                       .role_surface_override_supported = false,
                                       .host_policy_permits             = false,
                                       .observation                     = std::nullopt,
                                       .now                             = "2026-08-15T00:00:00Z"};
  REQUIRE(reg::reasons(reg::evaluate_eligibility(nothing)) ==
          std::vector<reg::eligibility_reason>{
              reg::eligibility_reason::provider_cli_unavailable, reg::eligibility_reason::exact_spawn_unverified,
              reg::eligibility_reason::role_tier_not_bound, reg::eligibility_reason::role_surface_override_unsupported,
              reg::eligibility_reason::host_policy_denied, reg::eligibility_reason::host_observation_expired});
}

TEST_CASE("models.registry: freshness compares timestamps BYTEWISE, not chronologically", "[models]") {
  reg::host_observation obs{
      .id                  = 1,
      .candidate_id        = 1,
      .host_id             = "h",
      .observation_version = 1,
      .availability_       = reg::availability::available,
      .spawn_verification_ = reg::spawn_verification::verified,
      .evidence_ref        = "e",
      .captured_at         = "2026-08-01T00:00:00Z",
      .expires_at          = "2026-09-01T00:00:00Z",
  };
  auto with_now = [&](std::string_view now) {
    return reg::evaluate_eligibility({.enabled                         = true,
                                      .binding_present                 = true,
                                      .role_surface_override_supported = true,
                                      .host_policy_permits             = true,
                                      .observation                     = obs,
                                      .now                             = now});
  };

  REQUIRE(with_now("2026-08-15T00:00:00Z").observation_fresh);
  REQUIRE_FALSE(with_now("2026-10-15T00:00:00Z").observation_fresh);
  // The boundary is STRICT: an instant equal to `expires_at` is already stale.
  REQUIRE_FALSE(with_now("2026-09-01T00:00:00Z").observation_fresh);
  REQUIRE(with_now("2026-08-31T23:59:59Z").observation_fresh);

  // THE decisive case, and the one that proves this is a byte sort rather
  // than a date compare: a millisecond AFTER expiry reads as FRESH, because
  // at offset 19 the comparison hits '.' (0x2E) against 'Z' (0x5A) and stops.
  // A chronological implementation would report it expired. Oracle-verified
  // directly, not merely assumed to follow from the source:
  //
  //   p models registry eligibility --candidate 1 --host h --role coder \
  //       --tier medium --now 2026-09-01T00:00:00.001Z \
  //       --override-supported --policy-permits
  //     -> observation_fresh = true
  REQUIRE(with_now("2026-09-01T00:00:00.001Z").observation_fresh);

  // Same reason, from the other direction: a shorter PREFIX sorts before the
  // full value and reads as fresh even though it is not a comparable instant.
  // Oracle-verified: `--now 2026-09` -> observation_fresh = true.
  REQUIRE(with_now("2026-09").observation_fresh);
  // And a same-instant spelling with a different (larger-sorting) prefix
  // character reads as EXPIRED despite being chronologically earlier.
  obs.expires_at = "2026-09-01 00:00:00Z"; // space instead of 'T' (0x20 < 'T')
  REQUIRE_FALSE(with_now("2026-09-01T00:00:00Z").observation_fresh);
}

TEST_CASE("models.registry: identity verification is exact, with vendor checked first", "[models]") {
  using iv = reg::identity_verification;

  REQUIRE(reg::verify_actual_identity("anthropic", "claude-opus-5", "anthropic", "claude-opus-5") == iv::matched);
  REQUIRE(reg::verify_actual_identity("anthropic", "claude-opus-5", "anthropic", "claude-opus-4") == iv::candidate_mismatch);
  REQUIRE(reg::verify_actual_identity("anthropic", "claude-opus-5", "openai", "claude-opus-5") == iv::vendor_mismatch);

  // BOTH halves wrong reports the VENDOR mismatch: vendor is checked first,
  // and reporting the candidate would point at the wrong problem.
  REQUIRE(reg::verify_actual_identity("anthropic", "claude-opus-5", "openai", "gpt-9") == iv::vendor_mismatch);

  REQUIRE(reg::verify_actual_identity("anthropic", "claude-opus-5", std::nullopt, "claude-opus-5") ==
          iv::missing_actual_identity);
  REQUIRE(reg::verify_actual_identity("anthropic", "claude-opus-5", "anthropic", std::nullopt) == iv::missing_actual_identity);
  REQUIRE(reg::verify_actual_identity("anthropic", "claude-opus-5", std::nullopt, std::nullopt) == iv::missing_actual_identity);

  // NO aliasing and NO case folding: an alias is precisely how a host could
  // silently serve a different model than an experiment declared.
  REQUIRE(reg::verify_actual_identity("anthropic", "claude-opus-5", "Anthropic", "claude-opus-5") == iv::vendor_mismatch);
  REQUIRE(reg::verify_actual_identity("anthropic", "claude-opus-5", "anthropic", "Claude-Opus-5") == iv::candidate_mismatch);
  REQUIRE(reg::verify_actual_identity("anthropic", "claude-opus-5", "anthropic", "claude-opus-5 ") == iv::candidate_mismatch);

  REQUIRE(reg::identity_verification_to_text(iv::matched) == "matched");
  REQUIRE(reg::identity_verification_to_text(iv::missing_actual_identity) == "missing_actual_identity");
  REQUIRE(reg::identity_verification_to_text(iv::vendor_mismatch) == "vendor_mismatch");
  REQUIRE(reg::identity_verification_to_text(iv::candidate_mismatch) == "candidate_mismatch");
}

TEST_CASE("models.registry: every eligibility reason has a distinct wire spelling", "[models]") {
  using er = reg::eligibility_reason;
  REQUIRE(reg::eligibility_reason_to_text(er::provider_cli_unavailable) == "provider_cli_unavailable");
  REQUIRE(reg::eligibility_reason_to_text(er::exact_spawn_unverified) == "exact_spawn_unverified");
  REQUIRE(reg::eligibility_reason_to_text(er::role_tier_not_bound) == "role_tier_not_bound");
  REQUIRE(reg::eligibility_reason_to_text(er::role_surface_override_unsupported) == "role_surface_override_unsupported");
  REQUIRE(reg::eligibility_reason_to_text(er::host_policy_denied) == "host_policy_denied");
  REQUIRE(reg::eligibility_reason_to_text(er::host_observation_expired) == "host_observation_expired");
}
