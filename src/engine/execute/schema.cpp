/// @file schema.cpp
/// @brief `planar::engine::execute::schema` — pure JSON-decode of `<bin>
/// schema`'s flat catalog (plan 996, task 6125).
///
/// Port target: `zig/src/cmd/planar-execute/schema.zig`. As with `state`,
/// the shelling is `host.cpp`'s job (`run_allowlisted(hs, "planar-agent",
/// {"schema"})`); this file is the pure parse half.

module planar.engine_execute;

import std;
import planar.json_dom;

namespace planar::engine::execute::schema {

namespace {

namespace dom = planar::json_dom;

/// @brief Read a string array field. Absent, non-array, or an element that
/// is not a string all decode to (or contribute) nothing extra — matching
/// `ignore_unknown_fields`/default-field leniency in the oracle's parser,
/// which is deliberately permissive here since these arrays (`aliases`,
/// `subcommands`) are display-only.
/// @param obj The parent object.
/// @param key The field name.
/// @return The decoded strings; empty when the field is absent or malformed.
auto read_string_array(dom::json_value const& obj, std::string_view key) -> std::vector<std::string> {
  std::vector<std::string> out;
  auto const*              field = obj.find(key);
  if (field == nullptr || field->kind != dom::json_kind::array) {
    return out;
  }
  out.reserve(field->array.size());
  for (auto const& element : field->array) {
    if (element.kind == dom::json_kind::string) {
      out.push_back(element.string);
    }
  }
  return out;
}

/// @brief Parse one `flag_entry`. Port target: `schema.zig`'s `FlagEntry`.
///
/// Only `long` is required (mirroring the Zig struct, whose sole
/// non-defaulted field is `long`); every other field defaults leniently.
/// @param obj The flag object.
/// @return The decoded flag, or unset when `long` is missing or not a
/// string.
auto parse_flag_entry(dom::json_value const& obj) -> std::optional<flag_entry> {
  if (obj.kind != dom::json_kind::object) {
    return std::nullopt;
  }
  auto const* long_field = obj.find("long");
  if (long_field == nullptr || long_field->kind != dom::json_kind::string) {
    return std::nullopt;
  }
  flag_entry out;
  out.long_name = long_field->string;
  out.aliases   = read_string_array(obj, "aliases");
  // `short` is emitted as a JSON STRING by the etcli-zig emitter (e.g.
  // "v" for -v), never a number — see schema.zig's task-3235 regression
  // test. A present, non-string `short` is treated as absent rather than a
  // parse failure, matching the oracle's `?[]const u8` leniency.
  if (auto const* short_field = obj.find("short"); short_field != nullptr && short_field->kind == dom::json_kind::string) {
    out.short_name = short_field->string;
  }
  if (auto const* required_field = obj.find("required");
      required_field != nullptr && required_field->kind == dom::json_kind::boolean) {
    out.required = required_field->boolean;
  }
  if (auto const* desc_field = obj.find("description"); desc_field != nullptr && desc_field->kind == dom::json_kind::string) {
    out.description = desc_field->string;
  }
  return out;
}

/// @brief Parse one `command_entry`. Port target: `schema.zig`'s
/// `CommandEntry`.
/// @param obj The command object.
/// @return The decoded command, or unset when `name` or `command` is
/// missing or not a string, or a `flags` element is malformed.
auto parse_command_entry(dom::json_value const& obj) -> std::optional<command_entry> {
  if (obj.kind != dom::json_kind::object) {
    return std::nullopt;
  }
  auto const* name_field    = obj.find("name");
  auto const* command_field = obj.find("command");
  if (name_field == nullptr || name_field->kind != dom::json_kind::string || command_field == nullptr ||
      command_field->kind != dom::json_kind::string) {
    return std::nullopt;
  }
  command_entry out;
  out.name        = name_field->string;
  out.command     = command_field->string;
  out.subcommands = read_string_array(obj, "subcommands");
  if (auto const* hidden_field = obj.find("hidden"); hidden_field != nullptr && hidden_field->kind == dom::json_kind::boolean) {
    out.hidden = hidden_field->boolean;
  }
  if (auto const* flags_field = obj.find("flags"); flags_field != nullptr && flags_field->kind == dom::json_kind::array) {
    out.flags.reserve(flags_field->array.size());
    for (auto const& element : flags_field->array) {
      auto flag = parse_flag_entry(element);
      if (!flag) {
        return std::nullopt;
      }
      out.flags.push_back(std::move(*flag));
    }
  }
  return out;
}

} // namespace

auto parse_raw_schema(std::string_view json) -> std::expected<raw_schema, schema_parse_error> {
  auto parsed = dom::parse_json(json);
  if (!parsed || parsed->kind != dom::json_kind::object) {
    return std::unexpected(schema_parse_error::malformed);
  }
  auto const* version_field  = parsed->find("schemaVersion");
  auto const* layout_field   = parsed->find("layout");
  auto const* root_field     = parsed->find("root");
  auto const* commands_field = parsed->find("commands");
  if (version_field == nullptr || version_field->kind != dom::json_kind::integer || layout_field == nullptr ||
      layout_field->kind != dom::json_kind::string || root_field == nullptr || root_field->kind != dom::json_kind::string ||
      commands_field == nullptr || commands_field->kind != dom::json_kind::array) {
    return std::unexpected(schema_parse_error::malformed);
  }

  raw_schema out;
  out.schema_version = static_cast<std::uint32_t>(version_field->integer);
  out.layout         = layout_field->string;
  out.root           = root_field->string;
  out.commands.reserve(commands_field->array.size());
  for (auto const& element : commands_field->array) {
    auto cmd = parse_command_entry(element);
    if (!cmd) {
      return std::unexpected(schema_parse_error::malformed);
    }
    out.commands.push_back(std::move(*cmd));
  }
  return out;
}

} // namespace planar::engine::execute::schema
