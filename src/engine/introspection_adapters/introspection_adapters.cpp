/// @file introspection_adapters.cpp
/// @brief Implementation of `planar.engine.introspection_adapters` (plan
/// 996, tasks 6102 and 6352). See introspection_adapters.cppm for scope.

module planar.engine.introspection_adapters;

import std;
import planar.core.check;
import planar.json_dom;
import planar.introspection_preview;

namespace planar::engine::introspection_adapters {

namespace {

namespace jd = planar::json_dom;

// ===========================================================================
// JSON value accessors — mirrors introspection_adapters.zig's
// stringValue/integerValue/boolValue/objectValue/integerEquals/stringEquals.
// ===========================================================================

auto string_value(const jd::json_value* v) -> std::optional<std::string_view> {
  if (v == nullptr || v->kind != jd::json_kind::string) {
    return std::nullopt;
  }
  return std::string_view{v->string};
}

auto integer_value(const jd::json_value* v) -> std::optional<std::int64_t> {
  if (v == nullptr || v->kind != jd::json_kind::integer) {
    return std::nullopt;
  }
  return v->integer;
}

auto bool_value(const jd::json_value* v) -> bool {
  return v != nullptr && v->kind == jd::json_kind::boolean && v->boolean;
}

auto strict_bool_value(const jd::json_value* v) -> std::optional<bool> {
  if (v == nullptr || v->kind != jd::json_kind::boolean) {
    return std::nullopt;
  }
  return v->boolean;
}

auto optional_bool_value(const jd::json_value* v, bool default_value) -> std::optional<bool> {
  if (v == nullptr) {
    return default_value;
  }
  if (v->kind != jd::json_kind::boolean) {
    return std::nullopt;
  }
  return v->boolean;
}

/// @brief Add `delta` to `*value`, SATURATING at `std::uint32_t`'s max
/// rather than wrapping. Mirrors the oracle's `+|=` saturating-add, used
/// throughout `collectPreview` for every coverage tally and `signal.count`
/// (zig:132, 140-146, 716; verified against zig:148's
/// `std.debug.assert(coverageIsAccounted(cov))`, which a wrapped counter
/// would silently fail rather than plateau against). Practically
/// unreachable at current traffic volumes — the fix is here so the
/// divergence from the oracle is reproduced rather than merely documented.
auto saturating_add(std::uint32_t& value, std::uint32_t delta = 1) -> void {
  value = value > std::numeric_limits<std::uint32_t>::max() - delta ? std::numeric_limits<std::uint32_t>::max() : value + delta;
}

auto object_value(const jd::json_value* v) -> const jd::json_value* {
  if (v == nullptr || v->kind != jd::json_kind::object) {
    return nullptr;
  }
  return v;
}

auto integer_equals(const jd::json_value* v, std::int64_t expected) -> bool {
  auto const got = integer_value(v);
  return got.has_value() && *got == expected;
}

auto string_equals(const jd::json_value* v, std::string_view expected) -> bool {
  auto const got = string_value(v);
  return got.has_value() && *got == expected;
}

/// @brief Split `text` on runs of ASCII whitespace, mirroring
/// `std.mem.tokenizeAny(u8, text, " \t\r\n")`.
auto tokenize(std::string_view text) -> std::vector<std::string_view> {
  std::vector<std::string_view> words;
  std::size_t                   i     = 0;
  auto const                    is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  while (i < text.size()) {
    while (i < text.size() && is_ws(text[i])) {
      ++i;
    }
    if (i >= text.size()) {
      break;
    }
    std::size_t const start = i;
    while (i < text.size() && !is_ws(text[i])) {
      ++i;
    }
    words.push_back(text.substr(start, i - start));
  }
  return words;
}

/// @brief The final path component of `word`.
auto path_basename(std::string_view word) -> std::string_view {
  auto const slash = word.rfind('/');
  return slash == std::string_view::npos ? word : word.substr(slash + 1);
}

/// @brief Whether `word` is a `NAME=value` environment assignment.
auto is_env_assignment(std::string_view word) -> bool {
  auto const eq = word.find('=');
  if (eq == std::string_view::npos || eq == 0) {
    return false;
  }
  auto const is_name_char = [](char c) { return c == '_' || std::isalnum(static_cast<unsigned char>(c)) != 0; };
  return !std::isdigit(static_cast<unsigned char>(word[0])) && std::ranges::all_of(word.substr(0, eq), is_name_char);
}

/// @brief Whether `word` starts a shell redirection (`>f`, `2>&1`, `<f`).
auto is_redirection(std::string_view word) -> bool {
  std::size_t i = 0;
  while (i < word.size() && std::isdigit(static_cast<unsigned char>(word[i])) != 0) {
    ++i;
  }
  return i < word.size() && (word[i] == '<' || word[i] == '>');
}

/// @brief Whether a chain step may precede planar when joined to it by `&&`:
/// a step that only changes directory or sets the environment cannot fail
/// the chain with a status of its own that the transcript would then
/// attribute to planar.
auto is_trivial_step(const std::vector<std::string_view>& step) -> bool {
  if (step.empty()) {
    return false;
  }
  if (step[0] == "cd" || step[0] == "pushd" || step[0] == "export") {
    return true;
  }
  return std::ranges::all_of(step, is_env_assignment);
}

/// @brief The argument words after the `planar` executable in `command`, or
/// unset when the command is not one whose recorded exit status is planar's.
///
/// The command is split into steps at `;` and `&&`. Planar must be the LAST
/// step, so the chain's status is planar's. A step joined to planar by `;`
/// may be anything; every step joined by `&&` in the run directly before
/// planar must be trivial (`cd`, `pushd`, `export`, `NAME=value`), because a
/// failing `make &&` would end the chain with make's status. The planar step
/// may carry `env`/`NAME=value` prefixes and an executable spelled `planar`
/// or any path ending in `/planar`. Pipelines, command substitution,
/// here-documents, background jobs and a second planar step are rejected.
/// Only the planar step's words are returned.
auto planar_argv_tail(std::string_view command) -> std::optional<std::vector<std::string>> {
  if (command.find_first_of("|`\n\r") != std::string_view::npos || command.find("$(") != std::string_view::npos ||
      command.find("<<") != std::string_view::npos) {
    return std::nullopt;
  }
  // `;` for a sequence step, `&` for an `&&` step: the marker word that
  // introduces each step after the first.
  std::string spaced;
  for (std::size_t i = 0; i < command.size(); ++i) {
    char const c = command[i];
    if (c == ';') {
      spaced += " ; ";
    } else if (c == '&') {
      if (i + 1 < command.size() && command[i + 1] == '&') {
        spaced += " && ";
        ++i;
      } else if (i > 0 && command[i - 1] == '>') {
        spaced += c; // the `&` of `2>&1`
      } else {
        return std::nullopt; // a background job
      }
    } else {
      spaced += c;
    }
  }
  std::vector<std::vector<std::string_view>> steps(1);
  std::vector<bool>                          joined_by_and{false}; // how each step is joined to the one before it
  for (auto const word : tokenize(spaced)) {
    if (word == ";" || word == "&&") {
      steps.emplace_back();
      joined_by_and.push_back(word == "&&");
    } else {
      steps.back().push_back(word);
    }
  }
  std::optional<std::size_t> planar_step;
  std::size_t                executable_at = 0;
  for (std::size_t index = 0; index < steps.size(); ++index) {
    auto const& step = steps[index];
    std::size_t at   = 0;
    while (at < step.size() && (step[at] == "env" || is_env_assignment(step[at]))) {
      ++at;
    }
    if (at < step.size() && path_basename(step[at]) == "planar") {
      if (planar_step.has_value()) {
        return std::nullopt;
      }
      planar_step   = index;
      executable_at = at;
    }
  }
  if (!planar_step.has_value() || *planar_step + 1 != steps.size()) {
    return std::nullopt;
  }
  for (std::size_t index = *planar_step; index > 0 && joined_by_and[index]; --index) {
    if (!is_trivial_step(steps[index - 1])) {
      return std::nullopt;
    }
  }
  std::vector<std::string> tail;
  auto const&              step = steps[*planar_step];
  for (std::size_t i = executable_at + 1; i < step.size() && !is_redirection(step[i]); ++i) {
    tail.emplace_back(step[i]);
  }
  if (tail.empty()) {
    return std::nullopt;
  }
  return tail;
}

/// @brief The signal verb path (`planar <verb path>`) for the planar
/// invocation in `command`, or unset when `command` is not one. The words are
/// never copied through: the verb path is what `resolve` (the live CLI
/// catalog rule) returns for them, so operator prose cannot reach a signal.
/// Without a resolver nothing is recognized.
auto planar_verb_path_from_command(std::string_view command, const verb_path_resolver* resolve) -> std::optional<std::string> {
  if (resolve == nullptr || !static_cast<bool>(*resolve)) {
    return std::nullopt;
  }
  auto const tail = planar_argv_tail(command);
  if (!tail.has_value()) {
    return std::nullopt;
  }
  auto const  resolved = (*resolve)(*tail);
  std::string path     = resolved.empty() ? std::string{"planar"} : "planar " + resolved;
  if (path.size() > 96) {
    return std::nullopt;
  }
  return path;
}

auto category_from_evidence(const jd::json_value* exit_value, bool retry, bool abandoned, bool gap) -> std::optional<category> {
  if (abandoned) {
    return category::abandonment;
  }
  if (gap) {
    return category::gap;
  }
  if (retry) {
    return category::retry;
  }
  auto const code = integer_value(exit_value);
  if (!code.has_value()) {
    return std::nullopt;
  }
  return *code == 0 ? std::nullopt : std::optional<category>{category::failure};
}

auto valid_verb_path(std::string_view path) -> bool {
  if (path.size() > 96 || !path.starts_with("planar ")) {
    return false;
  }
  if (path.find_first_of("=\"';&|\\/\n\r\t") != std::string_view::npos) {
    return false;
  }
  return path.find("--") == std::string_view::npos;
}

auto valid_timestamp(std::string_view ts) -> bool {
  return ts.size() >= 20 && ts.size() <= 35 && ts[4] == '-' && ts[10] == 'T';
}

auto time_bucket(std::string_view ts) -> std::string_view {
  return ts.substr(0, std::min<std::size_t>(ts.size(), 13));
}

// ===========================================================================
// Extraction
// ===========================================================================

struct extracted {
  std::string      verb_path;
  category         cat;
  std::string_view timestamp;
};

enum class extract_kind : std::uint8_t { normalized, ignored, malformed };

struct extract_result {
  extract_kind kind;
  extracted    value{}; // valid only when kind == normalized
};

constexpr extract_result k_ignored{extract_kind::ignored};
constexpr extract_result k_malformed{extract_kind::malformed};

auto finish_legacy(const jd::json_value* command_value, const jd::json_value* timestamp_value, std::optional<category> cat)
    -> extract_result {
  auto const command = string_value(command_value);
  if (!command.has_value()) {
    return k_malformed;
  }
  auto const timestamp = string_value(timestamp_value);
  if (!timestamp.has_value()) {
    return k_malformed;
  }
  if (!valid_verb_path(*command) || !valid_timestamp(*timestamp)) {
    return k_malformed;
  }
  if (!cat.has_value()) {
    return k_ignored;
  }
  return extract_result{extract_kind::normalized, extracted{std::string{*command}, *cat, *timestamp}};
}

struct pending_claude_tool {
  std::string id;
  std::string verb_path;
  bool        consumed = false;
};

/// @brief Per-source Claude tool_use/tool_result pairing state. Reset for
/// every raw source, matching the Zig original's `ExtractState`.
struct extract_state {
  std::vector<pending_claude_tool> claude_tools;
  const verb_path_resolver*        resolve = nullptr; ///< The transcript verb-path catalog rule, or null.
};

auto find_claude_tool(std::vector<pending_claude_tool>& tools, std::string_view id) -> pending_claude_tool* {
  for (auto& tool : tools) {
    if (tool.id == id) {
      return &tool;
    }
  }
  return nullptr;
}

auto extract_claude(extract_state& state, const jd::json_value& obj) -> extract_result {
  if (auto const* version = obj.find("version"); version != nullptr) {
    if (version->kind == jd::json_kind::integer || string_equals(obj.find("type"), "tool_result")) {
      if (!integer_equals(version, 1) || !string_equals(obj.find("type"), "tool_result")) {
        return k_malformed;
      }
      auto const* tool = object_value(obj.find("tool"));
      if (tool == nullptr) {
        return k_malformed;
      }
      return finish_legacy(tool->find("name"), obj.find("timestamp"),
                           category_from_evidence(obj.find("exit_code"), bool_value(obj.find("retry")), false,
                                                  bool_value(obj.find("invalid_flag")) || bool_value(obj.find("help_bounce"))));
    }
  }

  auto const record_type = string_value(obj.find("type"));
  if (!record_type.has_value()) {
    return k_malformed;
  }
  if (*record_type != "assistant" && *record_type != "user") {
    return k_ignored;
  }
  auto const timestamp = string_value(obj.find("timestamp"));
  if (!timestamp.has_value()) {
    return k_malformed;
  }
  if (!valid_timestamp(*timestamp)) {
    return k_malformed;
  }
  auto const* message = object_value(obj.find("message"));
  if (message == nullptr) {
    return k_malformed;
  }
  if (!string_equals(message->find("role"), *record_type)) {
    return k_malformed;
  }
  auto const* content_value = message->find("content");
  if (content_value == nullptr) {
    return k_malformed;
  }

  if (*record_type == "assistant") {
    if (content_value->kind == jd::json_kind::string) {
      return k_ignored;
    }
    if (content_value->kind != jd::json_kind::array) {
      return k_malformed;
    }
    // Validate the entire recognized envelope before mutating pairing
    // state, so a malformed sibling block cannot leave usable residue.
    for (auto const& block : content_value->array) {
      if (block.kind != jd::json_kind::object) {
        return k_malformed;
      }
      auto const block_type = string_value(block.find("type"));
      if (!block_type.has_value()) {
        return k_malformed;
      }
      if (*block_type == "text") {
        if (!string_value(block.find("text")).has_value()) {
          return k_malformed;
        }
        continue;
      }
      if (*block_type != "tool_use") {
        continue;
      }
      if (!string_value(block.find("id")).has_value()) {
        return k_malformed;
      }
      auto const name = string_value(block.find("name"));
      if (!name.has_value()) {
        return k_malformed;
      }
      auto const* input = object_value(block.find("input"));
      if (input == nullptr) {
        return k_malformed;
      }
      if (*name == "Bash") {
        if (!string_value(input->find("command")).has_value()) {
          return k_malformed;
        }
      }
    }
    for (auto const& block : content_value->array) {
      if (!string_equals(block.find("type"), "tool_use")) {
        continue;
      }
      auto const  id    = *string_value(block.find("id"));
      auto const  name  = *string_value(block.find("name"));
      auto const* input = object_value(block.find("input"));
      if (name != "Bash") {
        continue;
      }
      auto const command   = *string_value(input->find("command"));
      auto const verb_path = planar_verb_path_from_command(command, state.resolve);
      if (!verb_path.has_value()) {
        continue;
      }
      if (find_claude_tool(state.claude_tools, id) != nullptr) {
        continue;
      }
      if (state.claude_tools.size() == k_default_max_records) {
        continue;
      }
      state.claude_tools.push_back(pending_claude_tool{std::string{id}, *verb_path, false});
    }
    return k_ignored;
  }

  // record_type == "user"
  if (content_value->kind == jd::json_kind::string) {
    return k_ignored;
  }
  if (content_value->kind != jd::json_kind::array) {
    return k_malformed;
  }
  for (auto const& block : content_value->array) {
    if (block.kind != jd::json_kind::object) {
      return k_malformed;
    }
    auto const block_type = string_value(block.find("type"));
    if (!block_type.has_value()) {
      return k_malformed;
    }
    if (*block_type == "text") {
      if (!string_value(block.find("text")).has_value()) {
        return k_malformed;
      }
      continue;
    }
    if (*block_type != "tool_result") {
      continue;
    }
    if (!string_value(block.find("tool_use_id")).has_value()) {
      return k_malformed;
    }
    if (!optional_bool_value(block.find("is_error"), false).has_value()) {
      return k_malformed;
    }
  }
  for (auto const& block : content_value->array) {
    if (!string_equals(block.find("type"), "tool_result")) {
      continue;
    }
    auto const tool_use_id = *string_value(block.find("tool_use_id"));
    auto const is_error    = *optional_bool_value(block.find("is_error"), false);
    auto*      tool        = find_claude_tool(state.claude_tools, tool_use_id);
    if (tool == nullptr) {
      continue;
    }
    if (tool->consumed) {
      continue;
    }
    tool->consumed = true;
    if (!is_error) {
      return k_ignored;
    }
    return extract_result{extract_kind::normalized, extracted{tool->verb_path, category::failure, *timestamp}};
  }
  return k_ignored;
}

/// @brief The Codex `event_msg` arm: an `item_completed` `CommandExecution`
/// of a planar command with failure evidence (a non-zero `exit_code` or
/// `status: "failed"`) is a failure signal. The command array is either an
/// argv or a shell wrapper (`bash -lc "<script>"`); both go through the same
/// recognizer as Claude's Bash commands.
auto extract_codex_event(const extract_state& state, const jd::json_value& obj, std::string_view timestamp) -> extract_result {
  auto const* payload = object_value(obj.find("payload"));
  if (payload == nullptr || !string_equals(payload->find("type"), "item_completed")) {
    return k_ignored;
  }
  auto const* item = object_value(payload->find("item"));
  if (item == nullptr) {
    return k_malformed;
  }
  if (!string_equals(item->find("type"), "CommandExecution")) {
    return k_ignored;
  }
  auto const* command = item->find("command");
  if (command == nullptr || command->kind != jd::json_kind::array) {
    return k_malformed;
  }
  std::vector<std::string_view> argv;
  for (auto const& element : command->array) {
    auto const word = string_value(&element);
    if (!word.has_value()) {
      return k_malformed;
    }
    argv.push_back(*word);
  }
  auto const* exit_value = item->find("exit_code");
  if (exit_value != nullptr && !integer_value(exit_value).has_value()) {
    return k_malformed;
  }
  bool const failed = (exit_value != nullptr && *integer_value(exit_value) != 0) || string_equals(item->find("status"), "failed");
  if (!failed) {
    return k_ignored;
  }
  std::string script;
  auto const  shell = argv.empty() ? std::string_view{} : path_basename(argv[0]);
  if (argv.size() >= 3 && (shell == "bash" || shell == "sh" || shell == "zsh") && argv[1].size() >= 2 && argv[1][0] == '-' &&
      argv[1].back() == 'c') {
    script = std::string{argv[2]};
  } else {
    for (auto const word : argv) {
      script += script.empty() ? "" : " ";
      script += word;
    }
  }
  auto const verb_path = planar_verb_path_from_command(script, state.resolve);
  if (!verb_path.has_value()) {
    return k_ignored;
  }
  return extract_result{extract_kind::normalized, extracted{*verb_path, category::failure, timestamp}};
}

auto extract_codex(const extract_state& state, const jd::json_value& obj) -> extract_result {
  if (obj.find("schema_version") != nullptr || obj.find("event") != nullptr) {
    if (!integer_equals(obj.find("schema_version"), 1) || !string_equals(obj.find("event"), "command_execution")) {
      return k_malformed;
    }
    return finish_legacy(obj.find("command_name"), obj.find("timestamp"),
                         category_from_evidence(obj.find("exit_code"), bool_value(obj.find("retry_of_previous")), false,
                                                bool_value(obj.find("invalid_flag")) || bool_value(obj.find("help_bounce"))));
  }
  auto const record_type = string_value(obj.find("type"));
  if (!record_type.has_value()) {
    return k_malformed;
  }
  auto const timestamp = string_value(obj.find("timestamp"));
  if (!timestamp.has_value() || !valid_timestamp(*timestamp)) {
    return k_malformed;
  }
  if (*record_type == "event_msg") {
    return extract_codex_event(state, obj, *timestamp);
  }
  if (*record_type != "response_item") {
    return k_ignored;
  }
  auto const* payload = object_value(obj.find("payload"));
  if (payload == nullptr) {
    return k_malformed;
  }
  auto const payload_type = string_value(payload->find("type"));
  if (!payload_type.has_value()) {
    return k_malformed;
  }
  if (*payload_type == "custom_tool_call") {
    if (!string_value(payload->find("call_id")).has_value() || !string_value(payload->find("name")).has_value() ||
        !string_value(payload->find("input")).has_value()) {
      return k_malformed;
    }
  } else if (*payload_type == "custom_tool_call_output") {
    auto const* output = payload->find("output");
    if (!string_value(payload->find("call_id")).has_value() || output == nullptr ||
        (output->kind != jd::json_kind::string && output->kind != jd::json_kind::array)) {
      return k_malformed;
    }
  } else if (*payload_type == "function_call") {
    if (!string_value(payload->find("name")).has_value()) {
      return k_malformed;
    }
    if (!string_value(payload->find("arguments")).has_value()) {
      return k_malformed;
    }
    if (!string_value(payload->find("call_id")).has_value()) {
      return k_malformed;
    }
  } else if (*payload_type == "function_call_output") {
    if (!string_value(payload->find("call_id")).has_value()) {
      return k_malformed;
    }
    // Current Codex writes `output` as a string or as an array of content items.
    auto const* output = payload->find("output");
    if (output == nullptr || (output->kind != jd::json_kind::string && output->kind != jd::json_kind::array)) {
      return k_malformed;
    }
  }
  return k_ignored;
}

auto extract_copilot(const jd::json_value& obj) -> extract_result {
  if (obj.find("version") != nullptr || obj.find("kind") != nullptr) {
    if (!string_equals(obj.find("version"), "1") || !string_equals(obj.find("kind"), "shell_result")) {
      return k_malformed;
    }
    auto const* command = object_value(obj.find("command"));
    if (command == nullptr) {
      return k_malformed;
    }
    bool const abandoned = string_equals(obj.find("status"), "abandoned");
    return finish_legacy(command->find("name"), obj.find("time"),
                         category_from_evidence(obj.find("exit_code"), bool_value(obj.find("retry")), abandoned,
                                                bool_value(obj.find("invalid_flag")) || bool_value(obj.find("help_bounce"))));
  }
  auto const record_type = string_value(obj.find("type"));
  if (!record_type.has_value()) {
    return k_malformed;
  }
  auto const timestamp = string_value(obj.find("timestamp"));
  if (!timestamp.has_value() || !valid_timestamp(*timestamp)) {
    return k_malformed;
  }
  if (*record_type != "tool.execution_start" && *record_type != "tool.execution_complete") {
    return k_ignored;
  }
  auto const* data = object_value(obj.find("data"));
  if (data == nullptr) {
    return k_malformed;
  }
  if (!string_value(data->find("toolCallId")).has_value()) {
    return k_malformed;
  }
  if (*record_type == "tool.execution_start") {
    if (!string_value(data->find("toolName")).has_value()) {
      return k_malformed;
    }
    if (object_value(data->find("arguments")) == nullptr) {
      return k_malformed;
    }
  } else {
    if (!strict_bool_value(data->find("success")).has_value()) {
      return k_malformed;
    }
    if (data->find("result") == nullptr) {
      return k_malformed;
    }
  }
  return k_ignored;
}

auto extract_cli_log(const jd::json_value& obj) -> extract_result {
  if (!integer_equals(obj.find("schema"), 1) || !string_equals(obj.find("kind"), "cli_invocation")) {
    return k_malformed;
  }
  return finish_legacy(obj.find("verb_path"), obj.find("recorded_at"),
                       category_from_evidence(obj.find("exit_code"), bool_value(obj.find("retry")), false,
                                              string_equals(obj.find("error_category"), "usage")));
}

auto extract(extract_state& state, vendor v, const jd::json_value& value) -> extract_result {
  if (value.kind != jd::json_kind::object) {
    return k_malformed;
  }
  switch (v) {
  case vendor::claude:
    return extract_claude(state, value);
  case vendor::codex:
    return extract_codex(state, value);
  case vendor::copilot:
    return extract_copilot(value);
  case vendor::cli_log:
    return extract_cli_log(value);
  }
  return k_malformed;
}

// ===========================================================================
// Aggregation / dedup / sort
// ===========================================================================

/// @brief Merge `item` into `signals`, or append a new bucket. Returns
/// `false` only when a NEW bucket was needed and the evidence cap was
/// already reached (the caller then counts the line as `capped` rather
/// than `normalized`).
auto add_aggregate(std::vector<signal_row>& signals, vendor v, const extracted& item) -> bool {
  auto const bucket = time_bucket(item.timestamp);
  for (auto& signal : signals) {
    if (signal.v == v && signal.cat == item.cat && signal.verb_path == item.verb_path &&
        time_bucket(signal.first_seen) == bucket) {
      saturating_add(signal.count);
      if (item.timestamp < signal.first_seen) {
        signal.first_seen = std::string{item.timestamp};
      }
      if (item.timestamp > signal.last_seen) {
        signal.last_seen = std::string{item.timestamp};
      }
      return true;
    }
  }
  if (signals.size() == k_max_evidence_buckets) {
    return false;
  }
  signals.push_back(signal_row{
      .v          = v,
      .verb_path  = item.verb_path,
      .cat        = item.cat,
      .count      = 1,
      .first_seen = std::string{item.timestamp},
      .last_seen  = std::string{item.timestamp},
  });
  return true;
}

auto has_authoritative_cli(const std::vector<signal_row>& signals, const signal_row& candidate) -> bool {
  for (auto const& signal : signals) {
    if (signal.v == vendor::cli_log && signal.cat == candidate.cat && signal.verb_path == candidate.verb_path &&
        time_bucket(signal.first_seen) == time_bucket(candidate.first_seen)) {
      return true;
    }
  }
  return false;
}

auto remove_cli_duplicates(std::vector<signal_row>& signals) -> void {
  std::erase_if(signals, [&signals](signal_row const& candidate) {
    return candidate.v != vendor::cli_log && has_authoritative_cli(signals, candidate);
  });
}

auto signal_less_than(const signal_row& a, const signal_row& b) -> bool {
  if (a.v != b.v) {
    return static_cast<std::uint8_t>(a.v) < static_cast<std::uint8_t>(b.v);
  }
  if (a.verb_path != b.verb_path) {
    return a.verb_path < b.verb_path;
  }
  if (a.cat != b.cat) {
    return static_cast<std::uint8_t>(a.cat) < static_cast<std::uint8_t>(b.cat);
  }
  return a.first_seen < b.first_seen;
}

auto coverage_less_than(const coverage_row& a, const coverage_row& b) -> bool {
  return static_cast<std::uint8_t>(a.v) < static_cast<std::uint8_t>(b.v);
}

/// @brief Whether a coverage row's tallies are internally consistent:
/// every scanned line landed in exactly one of normalized/ignored/
/// malformed/capped. Mirrors the oracle's `coverageIsAccounted` (zig:832),
/// asserted immediately before every non-early-exit coverage row is
/// appended (zig:148).
auto coverage_accounted(const coverage_row& cov) -> bool {
  return static_cast<std::uint64_t>(cov.scanned) ==
         static_cast<std::uint64_t>(cov.normalized) + static_cast<std::uint64_t>(cov.ignored) +
             static_cast<std::uint64_t>(cov.malformed) + static_cast<std::uint64_t>(cov.capped);
}

} // namespace

auto collect_preview(std::span<const raw_source> sources, const verb_path_resolver& resolve) -> preview {
  std::vector<signal_row>   signals;
  std::vector<coverage_row> coverage;

  for (auto const& source : sources) {
    extract_state state;
    state.resolve = &resolve;
    coverage_row cov{.v                    = source.v,
                     .state                = coverage_state::observed,
                     .bytes_read           = source.bytes_read,
                     .files_partial        = source.files_partial,
                     .files_skipped_cap    = source.files_skipped_cap,
                     .files_skipped_window = source.files_skipped_window,
                     .bytes_scanned        = source.bytes_scanned,
                     .bytes_retained       = source.bytes_read,
                     .lines_oversize       = source.lines_oversize,
                     .results_unpaired     = source.results_unpaired};
    if (!source.enabled) {
      cov.state = coverage_state::disabled;
      coverage.push_back(cov);
      continue;
    }
    if (!source.available) {
      cov.state = coverage_state::unavailable;
      coverage.push_back(cov);
      continue;
    }

    std::string_view remaining = source.jsonl;
    bool             more      = true;
    while (more) {
      auto const nl   = remaining.find('\n');
      auto const line = nl == std::string_view::npos ? remaining : remaining.substr(0, nl);
      if (nl == std::string_view::npos) {
        more = false;
      } else {
        remaining = remaining.substr(nl + 1);
      }

      std::string_view trimmed = line;
      while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t' || trimmed.front() == '\r')) {
        trimmed.remove_prefix(1);
      }
      while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\r')) {
        trimmed.remove_suffix(1);
      }
      if (trimmed.empty()) {
        continue;
      }
      saturating_add(cov.scanned);
      auto parsed = jd::parse_json(trimmed);
      if (!parsed.has_value()) {
        saturating_add(cov.malformed);
        continue;
      }
      auto const result = extract(state, source.v, *parsed);
      switch (result.kind) {
      case extract_kind::normalized:
        if (add_aggregate(signals, source.v, result.value)) {
          saturating_add(cov.normalized);
        } else {
          saturating_add(cov.capped);
        }
        break;
      case extract_kind::ignored:
        saturating_add(cov.ignored);
        break;
      case extract_kind::malformed:
        saturating_add(cov.malformed);
        break;
      }
    }
    check(coverage_accounted(cov), "coverage_accounted(cov)");
    coverage.push_back(cov);
  }

  remove_cli_duplicates(signals);
  // STABLE: two coverage rows for the same vendor (e.g. two `cli_log`
  // sources, one disabled and one unavailable) must keep their relative
  // input order — `coverageLessThan`/`signalLessThan` only ever compare a
  // PREFIX of the full key, so an unstable sort could silently swap ties.
  // PINNED AT TASK 6348 by `coverage rows for one vendor keep INPUT order`,
  // which DOES flip this to `sort` and fail. Getting a discriminating fixture
  // took three attempts and the reason is worth recording, because the two
  // failures both LOOK like passes:
  //
  //   n=20, one vendor    SURVIVED -- introsort falls back to insertion sort
  //                       below ~30 elements, and insertion sort is stable.
  //   n=64, one vendor    SURVIVED -- an ALL-TIES range is degenerate for
  //                       introsort: it partitions to nothing and preserves
  //                       order whatever the algorithm.
  //   n=64, TWO vendors   KILLS -- interleaving forces a real partition, and
  //   interleaved         the ties then sit inside it.
  //
  // So a small or single-vendor fixture cannot tell `sort` from `stable_sort`
  // here at any size. Do not "simplify" that test back toward one vendor.
  std::ranges::stable_sort(signals, signal_less_than);
  std::ranges::stable_sort(coverage, coverage_less_than);

  std::vector<warning_row> warnings;
  for (auto const& cov : coverage) {
    if (cov.state == coverage_state::disabled) {
      warnings.push_back(warning_row{.v = cov.v, .kind = warning_kind::disabled});
    }
    if (cov.state == coverage_state::unavailable) {
      warnings.push_back(warning_row{.v = cov.v, .kind = warning_kind::unavailable});
    }
    if (cov.malformed != 0) {
      warnings.push_back(warning_row{.v = cov.v, .kind = warning_kind::malformed, .count = cov.malformed});
    }
    if (cov.capped != 0) {
      warnings.push_back(warning_row{.v = cov.v, .kind = warning_kind::evidence_cap, .count = cov.capped});
    }
  }

  return preview{.signals = std::move(signals), .coverage = std::move(coverage), .warnings = std::move(warnings)};
}

auto discover(bool enabled, std::string_view override_path, std::string_view builtin) -> std::optional<std::string_view> {
  if (!enabled) {
    return std::nullopt;
  }
  return override_path.empty() ? builtin : override_path;
}

// ===========================================================================
// Discovery half (task 6352): collect_preview_from_paths and
// collect_vendor_path. See introspection_adapters.cppm for the exported
// vocabulary this section builds on.
// ===========================================================================

namespace {

/// @brief Count non-blank lines in `bytes`. Mirrors the oracle's
/// `countRecords` (zig:423): the same JSONL "one record per non-blank
/// line" convention `collect_preview`'s own line-splitting loop uses.
auto count_records(std::string_view bytes) -> std::size_t {
  std::size_t count = 0;
  std::size_t start = 0;
  bool        more  = true;
  while (more) {
    auto const nl   = bytes.find('\n', start);
    auto const line = nl == std::string_view::npos ? bytes.substr(start) : bytes.substr(start, nl - start);
    if (nl == std::string_view::npos) {
      more = false;
    } else {
      start = nl + 1;
    }
    std::string_view trimmed = line;
    while (!trimmed.empty() && (trimmed.front() == ' ' || trimmed.front() == '\t' || trimmed.front() == '\r')) {
      trimmed.remove_prefix(1);
    }
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\t' || trimmed.back() == '\r')) {
      trimmed.remove_suffix(1);
    }
    if (!trimmed.empty()) {
      ++count;
    }
  }
  return count;
}

/// @brief Whether the fault seam wants `operation` to fail. An empty
/// (default-constructed) `fault` never fails anything.
auto fs_should_fail(const fs_fault& fault, vendor v, fs_operation operation, std::string_view path) -> bool {
  return static_cast<bool>(fault) && fault(v, operation, path);
}

// ---------------------------------------------------------------------------
// Scan: read wide, keep narrow (task diagnose-transcript-scan-wide-keep-narrow)
// ---------------------------------------------------------------------------

/// @brief The call-and-result prefilter for ONE transcript file.
///
/// A line is kept when it can be a planar tool call (it contains `planar` and
/// parses as the vendor's call shape, or already normalizes to a signal on its
/// own) or when it carries the id of a call kept earlier in the same file.
/// Every other line is dropped by the caller without being stored. State is per
/// file: pending ids never outlive the file.
class file_filter {
public:
  /// @param v The vendor whose line shapes apply.
  /// @param resolve The verb-path catalog rule that decides what a planar command is.
  file_filter(vendor v, const verb_path_resolver& resolve) : _v(v), _resolve(&resolve) {
  }

  /// @brief Decide whether `line` is kept. Registers the ids of a kept call as
  /// pending and clears an id when its result is kept.
  /// @param line One trimmed, non-empty line of at most `k_max_line_bytes`.
  /// @return Whether the line is kept.
  auto keep(std::string_view line) -> bool {
    bool paired = false;
    for (auto it = _pending.begin(); it != _pending.end();) {
      if (line.find(it->needle) != std::string_view::npos) {
        paired = true;
        it     = _pending.erase(it);
      } else {
        ++it;
      }
    }
    if (paired) {
      return true;
    }
    if (line.find("planar") != std::string_view::npos && is_call(line)) {
      return true;
    }
    note_discarded(line);
    return false;
  }

  /// @return Results seen whose call was not kept.
  [[nodiscard]] auto results_unpaired() const -> std::uint32_t {
    return _results_unpaired;
  }
  /// @return The most ids pending at once so far.
  [[nodiscard]] auto pending_high_water() const -> std::size_t {
    return _pending_high_water;
  }

private:
  struct pending_id {
    std::string needle; ///< The id as a quoted JSON string, for a substring search.
  };

  auto add_pending(std::string_view id) -> void {
    if (_pending.size() >= k_max_pending_ids) {
      // The id cannot be tracked, so its result will not pair: count it now and
      // remember it as seen, so the result itself is not counted a second time.
      saturating_add(_results_unpaired);
      remember_other(std::string{id});
      return;
    }
    _pending.push_back(pending_id{std::format("\"{}\"", id)});
    _pending_high_water = std::max(_pending_high_water, _pending.size());
  }

  /// Remember the id of a call that was not kept, within a bounded window, so
  /// its result is recognised as that call's and not counted unpaired.
  auto remember_other(std::string id) -> void {
    if (_other.contains(id)) {
      return;
    }
    if (_other_order.size() >= k_max_pending_ids) {
      _other.erase(_other_order.front());
      _other_order.pop_front();
    }
    _other.insert(id);
    _other_order.push_back(std::move(id));
  }

  auto settle_result(std::string_view id) -> void {
    if (auto it = _other.find(std::string{id}); it != _other.end()) {
      _other.erase(it);
      std::erase(_other_order, std::string{id});
      return;
    }
    saturating_add(_results_unpaired);
  }

  /// Every quoted value that follows `key` (which ends in `":"`), up to 256 bytes each.
  template <typename F> static auto for_each_value(std::string_view line, std::string_view key, F&& fn) -> void {
    std::size_t at = 0;
    while ((at = line.find(key, at)) != std::string_view::npos) {
      at += key.size();
      auto const end = line.find('"', at);
      if (end == std::string_view::npos) {
        return;
      }
      if (end - at <= 256 && end > at) {
        fn(line.substr(at, end - at));
      }
      at = end;
    }
  }

  /// Bookkeeping for a line that is not kept: note the ids of calls, and count a
  /// result that no kept or seen call explains.
  auto note_discarded(std::string_view line) -> void {
    switch (_v) {
    case vendor::claude:
      if (line.find("\"tool_result\"") != std::string_view::npos) {
        for_each_value(line, "\"tool_use_id\":\"", [this](std::string_view id) { settle_result(id); });
      }
      if (line.find("\"tool_use\"") != std::string_view::npos) {
        std::size_t at = 0;
        while ((at = line.find("\"type\":\"tool_use\"", at)) != std::string_view::npos) {
          at += 17;
          auto const window = line.substr(at, 512);
          auto const key    = window.find("\"id\":\"");
          if (key != std::string_view::npos) {
            auto const begin = key + 6;
            auto const end   = window.find('"', begin);
            if (end != std::string_view::npos && end > begin) {
              remember_other(std::string{window.substr(begin, end - begin)});
            }
          }
        }
      }
      break;
    case vendor::codex:
      if (line.find("_call_output\"") != std::string_view::npos) {
        for_each_value(line, "\"call_id\":\"", [this](std::string_view id) { settle_result(id); });
      } else if (line.find("_call\"") != std::string_view::npos) {
        for_each_value(line, "\"call_id\":\"", [this](std::string_view id) { remember_other(std::string{id}); });
      }
      break;
    case vendor::copilot:
      if (line.find("tool.execution_complete") != std::string_view::npos) {
        for_each_value(line, "\"toolCallId\":\"", [this](std::string_view id) { settle_result(id); });
      } else if (line.find("tool.execution_start") != std::string_view::npos) {
        for_each_value(line, "\"toolCallId\":\"", [this](std::string_view id) { remember_other(std::string{id}); });
      }
      break;
    case vendor::cli_log:
      break;
    }
  }

  /// Whether a line that mentions `planar` parses as this vendor's call shape
  /// (registering its ids), or is a record that normalizes to a signal by itself.
  auto is_call(std::string_view line) -> bool {
    auto parsed = jd::parse_json(line);
    if (!parsed.has_value() || parsed->kind != jd::json_kind::object) {
      return false;
    }
    extract_state scratch;
    scratch.resolve   = _resolve;
    auto const result = extract(scratch, _v, *parsed);
    if (_v == vendor::claude && !scratch.claude_tools.empty()) {
      for (auto const& tool : scratch.claude_tools) {
        add_pending(tool.id);
      }
      return true;
    }
    if (result.kind == extract_kind::normalized) {
      return true;
    }
    if (_v == vendor::codex) {
      return codex_function_call(*parsed);
    }
    if (_v == vendor::copilot) {
      return copilot_execution_start(*parsed);
    }
    return false;
  }

  /// A Codex `function_call` response item whose command is a planar invocation.
  auto codex_function_call(const jd::json_value& obj) -> bool {
    auto const* payload = object_value(obj.find("payload"));
    if (payload == nullptr || !string_equals(payload->find("type"), "function_call")) {
      return false;
    }
    auto const call_id = string_value(payload->find("call_id"));
    auto const args    = string_value(payload->find("arguments"));
    if (!call_id.has_value() || !args.has_value()) {
      return false;
    }
    auto const parsed = jd::parse_json(*args);
    if (!parsed.has_value() || parsed->kind != jd::json_kind::object) {
      return false;
    }
    if (!command_is_planar(parsed->find("cmd")) && !command_is_planar(parsed->find("command"))) {
      return false;
    }
    add_pending(*call_id);
    return true;
  }

  /// A Copilot `tool.execution_start` event whose arguments carry a planar command.
  auto copilot_execution_start(const jd::json_value& obj) -> bool {
    if (!string_equals(obj.find("type"), "tool.execution_start")) {
      return false;
    }
    auto const* data = object_value(obj.find("data"));
    if (data == nullptr) {
      return false;
    }
    auto const  id        = string_value(data->find("toolCallId"));
    auto const* arguments = object_value(data->find("arguments"));
    if (!id.has_value() || arguments == nullptr) {
      return false;
    }
    if (!command_is_planar(arguments->find("command")) && !command_is_planar(arguments->find("cmd"))) {
      return false;
    }
    add_pending(*id);
    return true;
  }

  /// Whether `value` (a command string, or an argv array of strings) is a planar invocation.
  auto command_is_planar(const jd::json_value* value) const -> bool {
    if (value == nullptr) {
      return false;
    }
    if (value->kind == jd::json_kind::string) {
      return planar_verb_path_from_command(value->string, _resolve).has_value();
    }
    if (value->kind != jd::json_kind::array) {
      return false;
    }
    std::string joined;
    for (auto const& word : value->array) {
      if (word.kind != jd::json_kind::string) {
        return false;
      }
      joined += joined.empty() ? "" : " ";
      joined += word.string;
    }
    if (value->array.size() >= 3) {
      auto const shell = path_basename(value->array[0].string);
      if ((shell == "bash" || shell == "sh" || shell == "zsh") && value->array[1].string.starts_with("-") &&
          value->array[1].string.ends_with("c")) {
        joined = value->array[2].string;
      }
    }
    return planar_verb_path_from_command(joined, _resolve).has_value();
  }

  vendor                          _v;
  const verb_path_resolver*       _resolve;
  std::vector<pending_id>         _pending;
  std::unordered_set<std::string> _other;
  std::deque<std::string>         _other_order;
  std::uint32_t                   _results_unpaired   = 0;
  std::size_t                     _pending_high_water = 0;
};

/// @brief Stream `file` forward, one line at a time, handing each trimmed
/// non-empty line to `on_line` until it returns false.
///
/// A line longer than `k_max_line_bytes` is never buffered past that size: it is
/// skipped and counted in `oversize`. With `drop_first_partial` the bytes up to
/// and including the first newline are discarded unread (a tail read starts
/// mid-record) and are not counted.
/// @param file The open file, already positioned.
/// @param limit The most bytes to read from the current position.
/// @param drop_first_partial Whether the first, possibly partial, line is dropped.
/// @param oversize Incremented once per skipped oversize line.
/// @param line_high_water Raised to the longest line held in the buffer.
/// @param on_line Receives each line; returns false to stop streaming.
/// @return The bytes streamed, excluding any dropped prefix.
template <typename F>
auto stream_lines(std::ifstream& file, std::uint64_t limit, bool drop_first_partial, std::uint32_t& oversize,
                  std::size_t& line_high_water, F&& on_line) -> std::uint64_t {
  std::array<char, 64 * 1024> buffer{};
  std::string                 current;
  bool                        overflow  = false;
  bool                        dropping  = drop_first_partial;
  bool                        go        = true;
  std::uint64_t               streamed  = 0;
  std::uint64_t               remaining = limit;

  auto const trimmed = [](std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
      text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
      text.remove_suffix(1);
    }
    return text;
  };

  while (go && remaining > 0 && file) {
    file.read(buffer.data(), static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), remaining)));
    auto const got = static_cast<std::size_t>(file.gcount());
    if (got == 0) {
      break;
    }
    remaining -= got;
    std::string_view chunk{buffer.data(), got};
    if (dropping) {
      auto const newline = chunk.find('\n');
      if (newline == std::string_view::npos) {
        continue;
      }
      chunk.remove_prefix(newline + 1);
      dropping = false;
    }
    streamed += chunk.size();
    while (!chunk.empty() && go) {
      auto const newline = chunk.find('\n');
      auto const piece   = newline == std::string_view::npos ? chunk : chunk.substr(0, newline);
      if (!overflow) {
        if (current.size() + piece.size() > k_max_line_bytes) {
          overflow = true;
          current.clear();
        } else {
          current.append(piece);
          line_high_water = std::max(line_high_water, current.size());
        }
      }
      if (newline == std::string_view::npos) {
        break;
      }
      chunk.remove_prefix(newline + 1);
      if (overflow) {
        saturating_add(oversize);
        overflow = false;
      } else if (auto const line = trimmed(current); !line.empty()) {
        go = on_line(line);
      }
      current.clear();
    }
  }
  if (go && !dropping) {
    if (overflow) {
      saturating_add(oversize);
    } else if (auto const line = trimmed(current); !line.empty()) {
      on_line(line);
    }
  }
  return streamed;
}

/// @brief One in-window transcript file.
struct candidate {
  std::string                     path;
  std::uintmax_t                  size = 0;
  std::filesystem::file_time_type mtime;
};

/// @brief What discovery found for one vendor, before any file is read.
struct vendor_plan {
  vendor                    v;
  std::optional<raw_source> settled;    ///< Set when the vendor needs no scan (disabled, unavailable, unsupported).
  std::vector<warning_row>  warnings;   ///< Warnings raised while discovering.
  std::vector<candidate>    candidates; ///< In-window files, newest first, ties by path.
  std::size_t               io_failures          = 0;
  std::uint32_t             files_skipped_window = 0;
  std::uint64_t             demand               = 0; ///< Total bytes of the in-window files.
};

/// @brief Discover one vendor's in-window transcript files without reading them.
/// Mirrors the oracle's `collectVendorPath` (zig:307-416) up to the file read.
///
/// TRANSLATION NOTE on the fault-injection seam: the oracle's Zig I/O layer
/// exposes `opendir` and `walk` as two distinct fallible steps
/// (`FsOperation.directory_open`/`.directory_walk`), which this port
/// preserves as two separate `fs_should_fail` checks even though
/// `std::filesystem` does not expose an "open a directory handle" step
/// distinct from iterating it — `directory_open`'s real operation below is
/// a non-recursive probe `directory_iterator`, `directory_walk`'s is the
/// actual `recursive_directory_iterator`. Both fault kinds fire on real I/O
/// failure either way; the seam split matters only to a test choosing
/// which of the two to inject, not to any oracle-observable behavior (the
/// caller never sees WHICH `FsOperation` failed — only the resulting
/// `warning_kind::unavailable`).
/// @param v Which vendor this call is for.
/// @param enabled Whether the vendor's adapter is configured on.
/// @param override_path The operator override path, or empty for the built-in.
/// @param home The operator's home directory.
/// @param builtin_rel The built-in path, relative to `home`.
/// @param jsonl_only When true (every transcript vendor), only `.jsonl`-suffixed
/// files count inside a directory; `false` takes every file.
/// @param window_start Files last modified before this are skipped and counted; unset admits all.
/// @param fault The fault-injection seam (tests only).
auto plan_vendor(vendor v, bool enabled, std::string_view override_path, std::string_view home, std::string_view builtin_rel,
                 bool jsonl_only, const std::optional<std::filesystem::file_time_type>& window_start, const fs_fault& fault)
    -> vendor_plan {
  vendor_plan plan{.v = v};
  if (!enabled) {
    plan.settled = raw_source{.v = v, .enabled = false};
    return plan;
  }
  auto const unavailable = [&](bool warn) {
    if (warn) {
      plan.warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
    }
    plan.settled = raw_source{.v = v, .available = false};
  };

  std::filesystem::path const builtin      = std::filesystem::path{std::string{home}} / builtin_rel;
  std::string                 selected_str = override_path.empty() ? builtin.string() : std::string{override_path};
  constexpr std::string_view  suffix{"/**/*.jsonl"};
  if (selected_str.size() >= suffix.size() && std::string_view{selected_str}.ends_with(suffix)) {
    selected_str.resize(selected_str.size() - suffix.size());
  }
  std::filesystem::path const selected{selected_str};

  if (fs_should_fail(fault, v, fs_operation::selected_stat, selected_str)) {
    unavailable(false);
    return plan;
  }
  std::error_code status_ec;
  auto const      status = std::filesystem::status(selected, status_ec);
  if (status_ec) {
    unavailable(false);
    return plan;
  }

  std::vector<std::string> paths;
  bool                     layout_scanned = false;
  if (status.type() == std::filesystem::file_type::regular) {
    paths.push_back(selected_str);
  } else if (status.type() == std::filesystem::file_type::directory) {
    if (fs_should_fail(fault, v, fs_operation::directory_open, selected_str)) {
      unavailable(true);
      return plan;
    }
    std::error_code                           open_ec;
    std::filesystem::directory_iterator const probe(selected, open_ec);
    if (open_ec) {
      unavailable(true);
      return plan;
    }
    if (fs_should_fail(fault, v, fs_operation::directory_walk, selected_str)) {
      unavailable(true);
      return plan;
    }
    // No `skip_permission_denied`: the oracle's `walker.next(io) catch`
    // (introspection_adapters.zig:352) pushes one `warning_kind::unavailable`
    // and BREAKS on a permission-denied subdirectory rather than silently
    // continuing past it — matched below by leaving the default iterator
    // options (`directory_options::none`) so a permission failure surfaces
    // through `entry_ec`/`walk_ec` instead of being swallowed by the
    // iterator itself.
    std::error_code                               walk_ec;
    std::filesystem::recursive_directory_iterator it(selected, walk_ec);
    if (walk_ec) {
      unavailable(true);
      return plan;
    }
    layout_scanned = true;
    std::filesystem::recursive_directory_iterator const end;
    while (it != end) {
      std::error_code entry_ec;
      // `symlink_status`, not `status`/`is_regular_file` (which follow a
      // symlink to its target). The oracle's `dir.walk` reports a symlink
      // entry as `.sym_link`, never `.file` (introspection_adapters.zig:352),
      // so a symlinked `.jsonl` is skipped there; matching that here means
      // testing the entry's OWN type, not what it points at.
      auto const entry_status = it->symlink_status(entry_ec);
      bool const is_file      = !entry_ec && entry_status.type() == std::filesystem::file_type::regular;
      if (entry_ec) {
        plan.warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
        break;
      }
      // `.filename().string().ends_with(...)`, not `.extension()`: a file
      // named exactly `.jsonl` has NO extension under
      // `std::filesystem::path` (a leading-dot filename is treated as the
      // stem, not an extension), while the oracle's `std.mem.endsWith`
      // matches it. `ends_with` on the filename reproduces the oracle for
      // that edge case.
      if (is_file && (!jsonl_only || it->path().filename().string().ends_with(".jsonl"))) {
        paths.push_back(it->path().string());
      }
      it.increment(entry_ec);
      if (entry_ec) {
        plan.warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
        break;
      }
    }
  } else {
    unavailable(false);
    return plan;
  }

  // A Copilot session-state directory that holds no JSONL file has no record
  // stream this adapter can read (the CLI writes YAML, Markdown and JSON
  // metadata there): report it unsupported instead of counting those lines.
  if (v == vendor::copilot && layout_scanned && paths.empty()) {
    plan.warnings.push_back(warning_row{.v = v, .kind = warning_kind::unsupported_layout});
    plan.settled = raw_source{.v = v, .available = false};
    return plan;
  }

  // Stat every candidate once: size and modification time drive the window
  // filter and the newest-first order. A stat failure is an I/O failure for
  // that file only.
  for (auto& path : paths) {
    if (fs_should_fail(fault, v, fs_operation::file_stat, path)) {
      ++plan.io_failures;
      plan.warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
      continue;
    }
    std::error_code size_ec;
    auto const      size = std::filesystem::file_size(path, size_ec);
    std::error_code time_ec;
    auto const      mtime = std::filesystem::last_write_time(path, time_ec);
    if (size_ec || time_ec) {
      ++plan.io_failures;
      plan.warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
      continue;
    }
    if (window_start.has_value() && mtime < *window_start) {
      ++plan.files_skipped_window;
      continue;
    }
    plan.demand += size;
    plan.candidates.push_back(candidate{.path = std::move(path), .size = size, .mtime = mtime});
  }

  // Newest first; equal modification times fall back to path order so two
  // runs over the same tree read the same files.
  std::ranges::sort(plan.candidates, [](const candidate& a, const candidate& b) {
    return a.mtime != b.mtime ? a.mtime > b.mtime : a.path < b.path;
  });
  return plan;
}

/// @brief Split the read budget evenly across the vendors that have files to
/// read. A vendor whose files total less than its even share takes only that
/// total; the rest is shared among the others (max-min fairness).
/// @param demands Per vendor, the bytes its in-window files total; zero for a vendor with nothing to read.
/// @param total The whole read budget.
/// @return Per vendor, the read budget it starts with.
auto split_scan_budget(const std::vector<std::uint64_t>& demands, std::uint64_t total) -> std::vector<std::uint64_t> {
  std::vector<std::size_t> order;
  for (std::size_t i = 0; i < demands.size(); ++i) {
    if (demands[i] > 0) {
      order.push_back(i);
    }
  }
  std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) { return demands[a] < demands[b]; });
  std::vector<std::uint64_t> shares(demands.size(), 0);
  std::uint64_t              left      = total;
  std::size_t                remaining = order.size();
  for (auto const index : order) {
    auto const share = left / remaining;
    shares[index]    = std::min(demands[index], share);
    left -= shares[index];
    --remaining;
  }
  return shares;
}

/// @brief The pools one vendor's scan draws from and the caps it reports against.
struct scan_pools {
  std::size_t& files_left;   ///< Remaining file count, shared across vendors.
  std::size_t& bytes_left;   ///< Remaining retained bytes, shared across vendors and the CLI log.
  std::size_t& records_left; ///< Remaining retained records, shared across vendors and the CLI log.
  std::size_t  byte_share;   ///< This vendor's share of the retained budget.
};

/// @brief Stream one vendor's in-window files newest first and keep the lines
/// that can be a planar call or the result of one. Appends exactly one
/// `raw_source` to `owned`.
/// @param plan The discovery result; a settled plan is appended as is.
/// @param scan_budget The read budget this vendor may spend.
/// @param[out] scan_spent The read-budget bytes actually streamed.
/// @param pools The shared retained, record and file pools.
/// @param resolve The verb-path catalog rule.
/// @param limits The seams reported to.
/// @param fault The fault-injection seam (tests only).
/// @param owned Sources collected so far.
/// @param warnings Extra warnings raised outside `collect_preview`'s own coverage-driven set.
auto scan_vendor(vendor_plan& plan, std::uint64_t scan_budget, std::uint64_t& scan_spent, scan_pools pools,
                 const verb_path_resolver& resolve, const collector_limits& limits, const fs_fault& fault,
                 std::vector<raw_source>& owned, std::vector<warning_row>& warnings) -> void {
  scan_spent   = 0;
  auto const v = plan.v;
  for (auto& warning : plan.warnings) {
    warnings.push_back(warning);
  }
  if (plan.settled.has_value()) {
    owned.push_back(std::move(*plan.settled));
    return;
  }

  // This vendor's share of the retained budget; unspent bytes stay in the shared pool.
  std::size_t   retained_left = std::min(pools.byte_share, pools.bytes_left);
  std::size_t   retained_used = 0;
  std::uint64_t scan_left     = scan_budget;

  std::string   combined;
  std::size_t   scanned_files     = 0;
  std::uint64_t bytes_retained    = 0;
  std::uint64_t bytes_scanned     = 0;
  std::uint32_t lines_oversize    = 0;
  std::uint32_t results_unpaired  = 0;
  std::uint32_t files_partial     = 0;
  std::uint32_t files_skipped_cap = 0;
  std::size_t   io_failures       = plan.io_failures;
  bool          scan_cap_hit      = false;
  bool          retained_cap_hit  = false;
  bool          record_cap_hit    = false;
  bool          stop              = false;

  for (std::size_t index = 0; index < plan.candidates.size(); ++index) {
    auto const& entry = plan.candidates[index];
    if (stop) {
      ++files_skipped_cap;
      continue;
    }
    if (pools.files_left == 0) {
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::file_cap});
      stop = true;
      ++files_skipped_cap;
      continue;
    }
    // Only the newest file may be read from its tail, and only when it alone
    // exceeds the read budget; every other file that does not fit is skipped whole.
    bool const oversize = entry.size > scan_left;
    bool const partial  = oversize && index == 0 && scan_left > 0;
    if (oversize && !partial) {
      scan_cap_hit = true;
      ++files_skipped_cap;
      continue;
    }
    if (fs_should_fail(fault, v, fs_operation::file_read, entry.path)) {
      ++io_failures;
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
      continue;
    }
    std::ifstream file(entry.path, std::ios::binary);
    if (!file) {
      ++io_failures;
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
      continue;
    }

    std::uint64_t limit = entry.size; // capped at the stat'd size so a growing file cannot overrun the budget
    if (partial) {
      // Start one byte before the tail so a tail that begins exactly on a record
      // boundary keeps its first line; otherwise the partial first line is dropped.
      file.seekg(static_cast<std::streamoff>(entry.size - scan_left - 1));
      limit = scan_left + 1;
    }

    file_filter     filter(v, resolve);
    scan_high_water high_water;
    auto const      streamed =
        stream_lines(file, limit, partial, lines_oversize, high_water.line_buffer, [&](std::string_view line) -> bool {
          if (!filter.keep(line)) {
            return true;
          }
          if (line.size() > retained_left) {
            retained_cap_hit = true;
            return false;
          }
          if (pools.records_left == 0) {
            record_cap_hit = true;
            return false;
          }
          if (limits.retain_hook) {
            limits.retain_hook(v, line);
          }
          combined += line;
          combined += '\n';
          retained_left -= line.size();
          retained_used += line.size();
          bytes_retained += line.size();
          pools.records_left -= 1;
          return true;
        });
    bytes_scanned += streamed;
    scan_left -= std::min<std::uint64_t>(scan_left, streamed);
    saturating_add(results_unpaired, filter.results_unpaired());
    if (limits.file_hook) {
      high_water.pending_ids = filter.pending_high_water();
      limits.file_hook(v, high_water);
    }
    pools.files_left -= 1;
    ++scanned_files;
    if (partial) {
      ++files_partial;
      scan_cap_hit = true;
    }
    if (retained_cap_hit || record_cap_hit) {
      stop = true;
    }
  }
  if (record_cap_hit) {
    warnings.push_back(warning_row{.v = v, .kind = warning_kind::record_cap});
  }
  if (retained_cap_hit) {
    warnings.push_back(warning_row{.v = v, .kind = warning_kind::byte_cap, .reason = warning_reason::retained});
  }
  if (scan_cap_hit) {
    warnings.push_back(warning_row{.v = v, .kind = warning_kind::byte_cap, .reason = warning_reason::scanned});
  }
  pools.bytes_left -= std::min(pools.bytes_left, retained_used);
  scan_spent = bytes_scanned;

  raw_source result{.v = v};
  if (scanned_files == 0 && io_failures != 0) {
    result.available = false;
  } else {
    result.jsonl = std::move(combined);
  }
  result.bytes_read           = bytes_retained;
  result.bytes_scanned        = bytes_scanned;
  result.lines_oversize       = lines_oversize;
  result.results_unpaired     = results_unpaired;
  result.files_partial        = files_partial;
  result.files_skipped_cap    = files_skipped_cap;
  result.files_skipped_window = plan.files_skipped_window;
  owned.push_back(std::move(result));
}

} // namespace

auto collect_preview_from_paths(const transcript_config& config, const std::optional<cli_log_adapter>& cli,
                                const verb_path_resolver& resolve, const collector_limits& limits, const fs_fault& fault)
    -> preview {
  std::vector<raw_source>  owned;
  std::vector<warning_row> extra_warnings;
  std::size_t              files_left   = limits.max_files;
  std::size_t              bytes_left   = limits.max_bytes;
  std::size_t              records_left = limits.max_records;
  // Each of the three transcript vendors and the CLI log gets a quarter of the
  // retained budget, so a large inventory for one vendor cannot starve another.
  std::size_t const byte_share = limits.max_bytes / 4;

  // Discover first, so the read budget can be split by what each vendor has to read.
  std::vector<vendor_plan> plans;
  plans.push_back(plan_vendor(vendor::claude, config.claude_enabled, config.claude_path, config.home_dir, ".claude/projects",
                              true, limits.window_start, fault));
  plans.push_back(plan_vendor(vendor::codex, config.codex_enabled, config.codex_path, config.home_dir, ".codex/sessions", true,
                              limits.window_start, fault));
  plans.push_back(plan_vendor(vendor::copilot, config.copilot_enabled, config.copilot_path, config.home_dir,
                              ".copilot/session-state", true, limits.window_start, fault));
  std::vector<std::uint64_t> demands;
  for (auto const& plan : plans) {
    demands.push_back(plan.settled.has_value() ? 0 : plan.demand);
  }
  auto const    shares = split_scan_budget(demands, limits.max_scan_bytes);
  std::uint64_t carry  = 0; // read budget a vendor left unspent, handed to the next
  for (std::size_t i = 0; i < plans.size(); ++i) {
    std::uint64_t spent = 0;
    scan_vendor(plans[i], shares[i] + carry, spent, scan_pools{files_left, bytes_left, records_left, byte_share}, resolve, limits,
                fault, owned, extra_warnings);
    carry = shares[i] + carry - std::min(shares[i] + carry, spent);
  }

  if (cli.has_value()) {
    if (!cli->enabled) {
      owned.push_back(raw_source{.v = vendor::cli_log, .enabled = false});
    } else {
      // Not `auto const`: `result.bytes` is moved out below
      // (`std::move(result.bytes)`), and a `const` binding would silently
      // defeat that move and copy the whole CLI JSONL buffer instead.
      auto result = cli->read ? cli->read(bytes_left) : cli_read_result{};
      if (result.status == cli_read_status::ok) {
        auto const records = count_records(result.bytes);
        if (result.bytes.size() > bytes_left) {
          owned.push_back(raw_source{.v = vendor::cli_log});
          extra_warnings.push_back(warning_row{.v = vendor::cli_log, .kind = warning_kind::byte_cap});
        } else if (records > records_left) {
          owned.push_back(raw_source{.v = vendor::cli_log});
          extra_warnings.push_back(warning_row{.v = vendor::cli_log, .kind = warning_kind::record_cap});
        } else {
          bytes_left -= result.bytes.size();
          records_left -= records;
          owned.push_back(raw_source{.v = vendor::cli_log, .jsonl = std::move(result.bytes)});
          if (result.truncated) {
            // The reader stopped at the byte budget (oldest rows dropped): the
            // source is observed, and the omitted-row count rides on the warning.
            extra_warnings.push_back(warning_row{
                .v    = vendor::cli_log,
                .kind = warning_kind::byte_cap,
                .count =
                    static_cast<std::uint32_t>(std::min<std::size_t>(result.omitted, std::numeric_limits<std::uint32_t>::max())),
            });
          }
        }
      } else {
        owned.push_back(raw_source{.v = vendor::cli_log, .available = false});
        if (result.status == cli_read_status::failed) {
          extra_warnings.push_back(warning_row{.v = vendor::cli_log, .kind = warning_kind::cli_adapter_failed});
        }
      }
    }
  } else {
    owned.push_back(raw_source{.v = vendor::cli_log, .available = false});
  }

  auto result = collect_preview(owned, resolve);
  if (!extra_warnings.empty()) {
    result.warnings.insert(result.warnings.end(), std::make_move_iterator(extra_warnings.begin()),
                           std::make_move_iterator(extra_warnings.end()));
  }
  return result;
}

} // namespace planar::engine::introspection_adapters
