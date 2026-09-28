/// @file profile.cpp
/// @brief Implementation of `planar.engine.models.profile`.

module planar.engine.models.profile;

import std;
import planar.engine.models.registry;
import planar.engine.models.ranking;

namespace planar::engine::models::profile {

namespace {

using namespace std::string_view_literals;

// ---------------------------------------------------------------------------
// Rule tables
//
// Storage for the `any_of` spans. These are file-scope arrays rather than
// braced initializers inside `work_type_rules()` because a `std::span` must
// outlive the call that returns it.
// ---------------------------------------------------------------------------

constexpr std::array schema_kinds{fact_kinds::migration_touched, fact_kinds::schema_version_contract,
                                  fact_kinds::constraint_or_index_redesign};

constexpr std::array architectural_kinds{fact_kinds::new_subsystem, fact_kinds::new_binary};

constexpr std::array engine_kinds{fact_kinds::transaction_change,        fact_kinds::concurrency_change,
                                  fact_kinds::ownership_change,          fact_kinds::security_change,
                                  fact_kinds::resource_lifecycle_change, fact_kinds::status_transition_change,
                                  fact_kinds::scope_resolution_change,   fact_kinds::capability_boundary_change};

constexpr std::array cli_kinds{fact_kinds::cli_surface_change};

constexpr std::array mechanical_kinds{fact_kinds::docs_only, fact_kinds::rename_or_format_only};

/// PRECEDENCE ORDER. Not the enum's declaration order — see the module header.
const std::array rules{
    work_type_rule{.id = "work-type.schema.v1"sv, .type = work_type::schema, .any_of = schema_kinds},
    work_type_rule{.id = "work-type.architectural.v1"sv, .type = work_type::architectural, .any_of = architectural_kinds},
    work_type_rule{.id = "work-type.engine.v1"sv, .type = work_type::engine, .any_of = engine_kinds},
    work_type_rule{.id = "work-type.cli.v1"sv, .type = work_type::cli, .any_of = cli_kinds},
    work_type_rule{.id = "work-type.mechanical.v1"sv, .type = work_type::mechanical, .any_of = mechanical_kinds},
};

constexpr std::array<std::array<std::string_view, 2>, 7> contradiction_pairs{{
    {fact_kinds::docs_only, fact_kinds::migration_touched},
    {fact_kinds::docs_only, fact_kinds::schema_version_contract},
    {fact_kinds::docs_only, fact_kinds::new_subsystem},
    {fact_kinds::docs_only, fact_kinds::new_binary},
    {fact_kinds::docs_only, fact_kinds::cli_surface_change},
    {fact_kinds::rename_or_format_only, fact_kinds::new_subsystem},
    {fact_kinds::rename_or_format_only, fact_kinds::schema_version_contract},
}};

constexpr std::array threshold_rows{
    threshold{.id = "complexity.touched-units.v1"sv, .metric = fact_kinds::touched_unit_count, .bounded_max = 2, .large_min = 12},
    threshold{
        .id = "complexity.validation-gates.v1"sv, .metric = fact_kinds::validation_gate_count, .bounded_max = 1, .large_min = 6},
};

/// The capacity metrics, in the order `compile` evaluates them. Order is
/// contract: the FIRST metric to reach its `large_min` sets the threshold id
/// and breaks the loop.
constexpr std::array capacity_metrics{fact_kinds::touched_unit_count, fact_kinds::validation_gate_count};

// ---------------------------------------------------------------------------
// Fact lookup
// ---------------------------------------------------------------------------

[[nodiscard]] auto find_fact(std::span<const fact> facts, std::string_view kind) -> const fact* {
  auto it = std::ranges::find(facts, kind, &fact::kind);
  return it == facts.end() ? nullptr : &*it;
}

/// True when the fact ASSERTS truth. A fact recorded as `"false"` or `"0"` is
/// evidence the property does NOT hold, and must not match a rule keyed on the
/// fact's mere presence. Every other text — including the empty string — is
/// an assertion, matching the oracle's negated-equality form exactly.
[[nodiscard]] auto asserts_true(const fact& value) -> bool {
  return !(value.text == "false" || value.text == "0");
}

[[nodiscard]] auto truthy_fact(std::span<const fact> facts, std::string_view kind) -> const fact* {
  const fact* found = find_fact(facts, kind);
  if (found == nullptr) {
    return nullptr;
  }
  return asserts_true(*found) ? found : nullptr;
}

/// Parse a capacity metric's reading.
///
/// Three details of `std::fmt.parseInt(i64, std::mem.trim(u8, f.text, " \t"),
/// 10)` are reproduced deliberately:
///
///   - The oracle trims only SPACE and TAB — NOT newlines or carriage
///     returns. `"3\n"` is a parse failure there, so it is one here.
///   - A parse failure is swallowed into "no reading" (`catch null`) rather
///     than failing the packet. A garbage capacity fact is invisible, not
///     fatal.
///   - Zig's `parseInt` ACCEPTS a leading `+`; `std::from_chars` does not.
///     The sign is stripped explicitly so `"+5"` reads as 5 in both.
[[nodiscard]] auto int_fact(std::span<const fact> facts, std::string_view kind) -> std::optional<std::int64_t> {
  const fact* found = find_fact(facts, kind);
  if (found == nullptr) {
    return std::nullopt;
  }
  std::string_view text  = found->text;
  const auto       first = text.find_first_not_of(" \t"sv);
  if (first == std::string_view::npos) {
    return std::nullopt;
  }
  text = text.substr(first, text.find_last_not_of(" \t"sv) - first + 1);

  // A leading `-` is left for `from_chars`, which handles it; only `+` needs
  // stripping.
  if (text.starts_with('+')) {
    text.remove_prefix(1);
  }
  if (text.empty()) {
    return std::nullopt;
  }

  std::int64_t out{};
  const auto*  begin = text.data();
  const auto*  end   = begin + text.size();
  auto [ptr, ec]     = std::from_chars(begin, end, out);
  if (ec != std::errc{} || ptr != end) {
    return std::nullopt;
  }
  return out;
}

[[nodiscard]] auto to_matched(const fact& value) -> matched_fact {
  return matched_fact{
      .kind               = value.kind,
      .locator            = value.locator,
      .source_entity_kind = value.provenance,
      .source_entity_id   = value.id,
      .source_digest      = value.current_digest,
      .text               = value.text,
  };
}

} // namespace

auto work_type_rules() -> std::span<const work_type_rule> {
  return rules;
}

auto contradictions() -> std::span<const std::array<std::string_view, 2>> {
  return contradiction_pairs;
}

auto thresholds() -> std::span<const threshold> {
  return threshold_rows;
}

auto threshold_for(std::string_view metric) -> std::optional<threshold> {
  auto it = std::ranges::find(threshold_rows, metric, &threshold::metric);
  if (it == threshold_rows.end()) {
    return std::nullopt;
  }
  return *it;
}

auto not_ready_reason_name(not_ready_reason reason) -> std::string_view {
  switch (reason) {
  case not_ready_reason::contradictory_facts:
    return "contradictory_facts"sv;
  case not_ready_reason::missing_required_fact:
    return "missing_required_fact"sv;
  }
  return {};
}

auto accept_tier(const profile& value, tier requested) -> std::optional<tier> {
  if (std::to_underlying(requested) < std::to_underlying(value.tier_floor)) {
    return std::nullopt;
  }
  return requested;
}

auto compile(std::span<const fact> facts) -> outcome {
  // 1. Contradictions first. A task asserting both "docs only" and "touches a
  //    migration" has broken evidence; classifying it either way would launder
  //    that contradiction into a cohort.
  for (const auto& pair : contradiction_pairs) {
    if (truthy_fact(facts, pair[0]) != nullptr && truthy_fact(facts, pair[1]) != nullptr) {
      return not_ready_reason::contradictory_facts;
    }
  }

  // 2. Acceptance completeness is a required fact for classification: the tier
  //    floor depends on whether the work is "complete", so its absence is not a
  //    default, it is a gap. PRESENCE is what is required here — a fact
  //    asserting `false` passes this check and is re-read at step 5.
  if (find_fact(facts, fact_kinds::acceptance_complete) == nullptr) {
    return not_ready_reason::missing_required_fact;
  }

  std::vector<matched_fact> matched;
  std::vector<citation>     cites;

  // 3. Work type by first-match precedence.
  work_type        type    = work_type::feature;
  std::string_view rule_id = default_rule_id;
  for (const auto& rule : rules) {
    bool matched_rule = false;
    for (const auto& kind : rule.any_of) {
      const fact* found = truthy_fact(facts, kind);
      if (found == nullptr) {
        continue;
      }
      type    = rule.type;
      rule_id = rule.id;
      matched.push_back(to_matched(*found));
      cites.push_back(citation{.rule_id = rule.id, .fact_kind = found->kind, .locator = found->locator});
      matched_rule = true;
      break;
    }
    if (matched_rule) {
      break;
    }
  }

  // 4. Complexity. Explicit risk short-circuits capacity: an operator stating
  //    the work is risky outranks it looking small.
  complexity       risk         = complexity::standard;
  std::string_view threshold_id = ""sv;
  if (const fact* explicit_ = truthy_fact(facts, fact_kinds::explicit_risk); explicit_ != nullptr) {
    risk = complexity::high_risk;
    matched.push_back(to_matched(*explicit_));
    cites.push_back(
        citation{.rule_id = "complexity.explicit-risk.v1"sv, .fact_kind = explicit_->kind, .locator = explicit_->locator});
  } else {
    bool bounded_possible = true;
    bool saw_capacity     = false;
    for (const auto& metric : capacity_metrics) {
      const auto value = int_fact(facts, metric);
      if (!value.has_value()) {
        continue;
      }
      saw_capacity        = true;
      const auto covering = threshold_for(metric);
      if (!covering.has_value()) {
        return policy_gap{.metric = metric, .threshold_id = std::nullopt};
      }
      // A populated capacity metric with no large boundary cannot be judged:
      // the policy cannot say whether this reading is excessive.
      if (!covering->large_min.has_value()) {
        return policy_gap{.metric = metric, .threshold_id = covering->id};
      }
      if (*value >= *covering->large_min) {
        risk              = complexity::high_risk;
        threshold_id      = covering->id;
        const fact* found = find_fact(facts, metric);
        matched.push_back(to_matched(*found));
        cites.push_back(citation{.rule_id = covering->id, .fact_kind = metric, .locator = found->locator});
        break;
      }
      // A missing bounded boundary does not fail the packet; it simply
      // prevents claiming `bounded`, which is the conservative read.
      if (!covering->bounded_max.has_value()) {
        bounded_possible = false;
        continue;
      }
      if (*value > *covering->bounded_max) {
        bounded_possible = false;
      }
    }
    if (risk != complexity::high_risk && saw_capacity && bounded_possible) {
      risk = complexity::bounded;
      // Note the oracle looks up `touched_unit_count`'s threshold here
      // UNCONDITIONALLY — even when the only populated metric was
      // `validation_gate_count`. The reported id is the touched-units row
      // either way. Reproduced deliberately; see this module's tests.
      if (const auto covering = threshold_for(fact_kinds::touched_unit_count); covering.has_value()) {
        threshold_id = covering->id;
      }
    }
  }

  // 5. Tier floor.
  //    small  — only for bounded AND complete work
  //    large  — high risk, or bounded high-judgment acceptance whose
  //             correctness is observable (small-looking but consequential)
  //    medium — everything else
  //
  //    `complete` re-reads `acceptance_complete` for TRUTH; step 2 required
  //    only its presence.
  const bool       complete   = truthy_fact(facts, fact_kinds::acceptance_complete) != nullptr;
  tier             tier_floor = tier::medium;
  std::string_view tier_rule  = "tier-floor.standard.v1"sv;
  if (risk == complexity::high_risk) {
    tier_floor = tier::large;
    tier_rule  = "tier-floor.high-risk.v1"sv;
  } else if (risk == complexity::bounded && truthy_fact(facts, fact_kinds::high_judgment_acceptance) != nullptr &&
             truthy_fact(facts, fact_kinds::observable_correctness) != nullptr) {
    tier_floor        = tier::large;
    tier_rule         = "tier-floor.bounded-high-judgment.v1"sv;
    const fact* found = find_fact(facts, fact_kinds::high_judgment_acceptance);
    matched.push_back(to_matched(*found));
    cites.push_back(citation{.rule_id = tier_rule, .fact_kind = found->kind, .locator = found->locator});
  } else if (risk == complexity::bounded && complete) {
    tier_floor = tier::small;
    tier_rule  = "tier-floor.bounded-complete.v1"sv;
  }

  return profile{
      .type                    = type,
      .work_type_rule_id       = rule_id,
      .rule_version_           = rule_version,
      .risk                    = risk,
      .complexity_threshold_id = threshold_id,
      .tier_floor              = tier_floor,
      .tier_floor_rule_id      = tier_rule,
      .matched_facts           = std::move(matched),
      .citations               = std::move(cites),
  };
}

} // namespace planar::engine::models::profile
