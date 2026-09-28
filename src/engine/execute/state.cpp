/// @file state.cpp
/// @brief `planar::engine::execute::state` — pure JSON-decode helpers over
/// the planner reads `ctx.brief` consumes (plan 996, task 6125).
///
/// Port target: `zig/src/cmd/planar-execute/state.zig`, restricted to the
/// slice `execute.cppm`'s `state` namespace documents (`planShow`,
/// `taskShow`, the `TaskPacket` family) — see that header for why the
/// oracle's other `state.zig` helpers have no consumer on the ported host
/// surface and are deliberately not ported.
///
/// Every function here is a PURE parse: no subprocess call, no allocator
/// arena, no lifetime hazard to document — `host.cpp` shells the binary via
/// `run_allowlisted` and hands this file the captured stdout by value.

module planar.engine_execute;

import std;
import planar.json_dom;

namespace planar::engine::execute::state {

namespace {

namespace dom = planar::json_dom;

/// @brief Read a required string field.
/// @param obj The parent object.
/// @param key The field name.
/// @return The string, or unset when absent or not a string.
auto req_string(dom::json_value const& obj, std::string_view key) -> std::optional<std::string> {
  auto const* field = obj.find(key);
  if (field == nullptr || field->kind != dom::json_kind::string) {
    return std::nullopt;
  }
  return field->string;
}

/// @brief Read an optional string field — absent or non-string is "not
/// present" (distinct from a parse failure); `null` is also "not present".
/// @param obj The parent object.
/// @param key The field name.
/// @return The string, or unset.
auto opt_string(dom::json_value const& obj, std::string_view key) -> std::optional<std::string> {
  auto const* field = obj.find(key);
  if (field == nullptr || field->kind != dom::json_kind::string) {
    return std::nullopt;
  }
  return field->string;
}

/// @brief Read a required integer field (accepts `integer` only — the wire
/// contract never emits an id as a float).
/// @param obj The parent object.
/// @param key The field name.
/// @return The integer, or unset when absent or not an integer.
auto req_int(dom::json_value const& obj, std::string_view key) -> std::optional<std::int64_t> {
  auto const* field = obj.find(key);
  if (field == nullptr || field->kind != dom::json_kind::integer) {
    return std::nullopt;
  }
  return field->integer;
}

/// @brief Read an optional integer field.
/// @param obj The parent object.
/// @param key The field name.
/// @return The integer, or unset.
auto opt_int(dom::json_value const& obj, std::string_view key) -> std::optional<std::int64_t> {
  auto const* field = obj.find(key);
  if (field == nullptr || field->kind != dom::json_kind::integer) {
    return std::nullopt;
  }
  return field->integer;
}

/// @brief Read a required bool field.
/// @param obj The parent object.
/// @param key The field name.
/// @return The bool, or unset when absent or not a bool.
auto req_bool(dom::json_value const& obj, std::string_view key) -> std::optional<bool> {
  auto const* field = obj.find(key);
  if (field == nullptr || field->kind != dom::json_kind::boolean) {
    return std::nullopt;
  }
  return field->boolean;
}

/// @brief Parse one `packet_evidence` row.
/// @param obj The row object.
/// @return The decoded row, or unset on a shape mismatch.
auto parse_packet_evidence(dom::json_value const& obj) -> std::optional<packet_evidence> {
  if (obj.kind != dom::json_kind::object) {
    return std::nullopt;
  }
  packet_evidence out;
  auto            kind           = req_string(obj, "kind");
  auto            id             = req_int(obj, "id");
  auto            locator        = req_string(obj, "locator");
  auto            text           = req_string(obj, "text");
  auto            source_digest  = req_string(obj, "source_digest");
  auto            current_digest = req_string(obj, "current_digest");
  auto            required       = req_bool(obj, "required");
  auto            covered        = req_bool(obj, "covered");
  auto            status         = req_string(obj, "status");
  auto            provenance     = req_string(obj, "provenance");
  if (!kind || !id || !locator || !text || !source_digest || !current_digest || !required || !covered || !status || !provenance) {
    return std::nullopt;
  }
  out.kind                         = std::move(*kind);
  out.id                           = *id;
  out.locator                      = std::move(*locator);
  out.text                         = std::move(*text);
  out.source_digest                = std::move(*source_digest);
  out.current_digest               = std::move(*current_digest);
  out.required                     = *required;
  out.covered                      = *covered;
  out.status                       = std::move(*status);
  out.provenance                   = std::move(*provenance);
  out.display_label                = opt_string(obj, "display_label").value_or("");
  out.materializer_version         = opt_string(obj, "materializer_version").value_or("");
  out.current_materializer_version = opt_string(obj, "current_materializer_version").value_or("");
  out.freshness                    = opt_string(obj, "freshness").value_or("current");
  return out;
}

/// @brief Parse an array of `packet_evidence` rows. An absent or non-array
/// field decodes to an empty vector, matching the Zig struct's `= &.{}`
/// default fields.
/// @param obj The parent object.
/// @param key The array field name.
/// @return The decoded rows, or unset when a present array contains a
/// malformed row.
auto parse_evidence_array(dom::json_value const& obj, std::string_view key) -> std::optional<std::vector<packet_evidence>> {
  std::vector<packet_evidence> out;
  auto const*                  field = obj.find(key);
  if (field == nullptr || field->kind != dom::json_kind::array) {
    return out;
  }
  out.reserve(field->array.size());
  for (auto const& element : field->array) {
    auto row = parse_packet_evidence(element);
    if (!row) {
      return std::nullopt;
    }
    out.push_back(std::move(*row));
  }
  return out;
}

} // namespace

auto parse_plan_show(std::string_view json) -> std::expected<plan_show, state_parse_error> {
  auto parsed = dom::parse_json(json);
  if (!parsed || parsed->kind != dom::json_kind::object) {
    return std::unexpected(state_parse_error::malformed);
  }
  auto id     = req_int(*parsed, "id");
  auto title  = req_string(*parsed, "title");
  auto status = req_string(*parsed, "status");
  if (!id || !title || !status) {
    return std::unexpected(state_parse_error::malformed);
  }
  plan_show out;
  out.id             = *id;
  out.title          = std::move(*title);
  out.status         = std::move(*status);
  out.slug           = opt_string(*parsed, "slug");
  out.parent_plan_id = opt_int(*parsed, "parent_plan_id");
  return out;
}

auto parse_task_show(std::string_view json) -> std::expected<task_show, state_parse_error> {
  auto parsed = dom::parse_json(json);
  if (!parsed || parsed->kind != dom::json_kind::object) {
    return std::unexpected(state_parse_error::malformed);
  }
  auto id     = req_int(*parsed, "id");
  auto status = req_string(*parsed, "status");
  if (!id || !status) {
    return std::unexpected(state_parse_error::malformed);
  }
  task_show out;
  out.id     = *id;
  out.status = std::move(*status);
  out.slug   = opt_string(*parsed, "slug");
  return out;
}

auto parse_task_packet(std::string_view json) -> std::expected<task_packet, state_parse_error> {
  auto parsed = dom::parse_json(json);
  if (!parsed || parsed->kind != dom::json_kind::object) {
    return std::unexpected(state_parse_error::malformed);
  }
  auto const* input_field = parsed->find("input");
  auto        digest      = req_string(*parsed, "digest");
  if (input_field == nullptr || input_field->kind != dom::json_kind::object || !digest) {
    return std::unexpected(state_parse_error::malformed);
  }
  auto const& input_obj = *input_field;

  auto task_id             = req_int(input_obj, "task_id");
  auto status              = req_string(input_obj, "status");
  auto title               = req_string(input_obj, "title");
  auto body                = req_string(input_obj, "body");
  auto next_action         = req_string(input_obj, "next_action");
  auto acceptance_criteria = req_string(input_obj, "acceptance_criteria");
  if (!task_id || !status || !title || !body || !next_action || !acceptance_criteria) {
    return std::unexpected(state_parse_error::malformed);
  }

  task_packet_input input;
  input.task_id             = *task_id;
  input.status              = std::move(*status);
  input.title               = std::move(*title);
  input.body                = std::move(*body);
  input.next_action         = std::move(*next_action);
  input.acceptance_criteria = std::move(*acceptance_criteria);

  struct field_slot {
    std::string_view              key;
    std::vector<packet_evidence>* dest;
  };
  std::array<field_slot, 11> const slots{{
      {"owning_plans", &input.owning_plans},
      {"anchor_plans", &input.anchor_plans},
      {"citations", &input.citations},
      {"decisions", &input.decisions},
      {"questions", &input.questions},
      {"scenarios", &input.scenarios},
      {"dependencies", &input.dependencies},
      {"touches", &input.touches},
      {"claims", &input.claims},
      {"validation_gates", &input.validation_gates},
      {"facts", &input.facts},
  }};
  for (auto const& slot : slots) {
    auto rows = parse_evidence_array(input_obj, slot.key);
    if (!rows) {
      return std::unexpected(state_parse_error::malformed);
    }
    *slot.dest = std::move(*rows);
  }

  task_packet out;
  out.input  = std::move(input);
  out.digest = std::move(*digest);

  auto const* reasons_field = parsed->find("reasons");
  if (reasons_field != nullptr && reasons_field->kind == dom::json_kind::array) {
    out.reasons.reserve(reasons_field->array.size());
    for (auto const& element : reasons_field->array) {
      if (element.kind != dom::json_kind::string) {
        return std::unexpected(state_parse_error::malformed);
      }
      out.reasons.push_back(element.string);
    }
  }
  return out;
}

} // namespace planar::engine::execute::state
