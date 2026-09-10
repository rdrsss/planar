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

// ===========================================================================
// Verb-path bounding — mirrors boundedPlanarVerbPath and its table.
// ===========================================================================

struct planar_verb_rule {
  std::string_view domain;
  /// Space-separated suffixes after `planar <domain>`; empty is a direct
  /// root leaf. Every entry corresponds to a leaf in the CLI command tree.
  std::span<const std::string_view> leaves;
};

// clang-format off
constexpr std::string_view k_leaves_annotate[]    = {"add", "show", "list", "update", "remove", "tag", "resolve", "dismiss", "archive", "bulk-resolve", "bulk-dismiss", "bulk-archive", "verify", "sweep"};
constexpr std::string_view k_leaves_artifact[]    = {"add", "show", "list", "update", "edit", "view", "diff", "review", "link"};
constexpr std::string_view k_leaves_assoc[]       = {"list", "create", "add", "remove", "members", "detect"};
constexpr std::string_view k_leaves_audit[]       = {"trail", "commits", "session", "publish-decision", "handoff-readiness"};
constexpr std::string_view k_leaves_bench[]       = {"start", "event", "touch", "harvest", "finish", "show"};
constexpr std::string_view k_leaves_capture[]     = {"session", "commits", "end", "note", "command", "file", "snapshot"};
constexpr std::string_view k_leaves_closure[]     = {"compute", "show"};
constexpr std::string_view k_leaves_empty[]       = {""};
constexpr std::string_view k_leaves_config[]      = {"show", "edit", "validate", "init", "path"};
constexpr std::string_view k_leaves_decision[]    = {"add", "show", "list", "accept", "supersede", "withdraw", "edit", "view", "diff", "review", "link"};
constexpr std::string_view k_leaves_ext[]         = {"register jira", "register github", "list", "test", "create", "propagate-one", "propagate"};
constexpr std::string_view k_leaves_feedback[]    = {"triage list", "triage show", "triage set"};
constexpr std::string_view k_leaves_groups[]      = {"recommend"};
constexpr std::string_view k_leaves_handoff[]     = {"create", "validate", "consume", "abandon", "list", "show"};
constexpr std::string_view k_leaves_links[]       = {"add", "list", "remove", "trail"};
constexpr std::string_view k_leaves_local[]       = {"list", "link", "unlink", "import", "migrate"};
constexpr std::string_view k_leaves_models[]      = {"list", "refresh", "routing", "apply"};
constexpr std::string_view k_leaves_plan[]        = {"create", "show", "list", "update", "edit", "view", "diff", "review", "link", "next", "recommend-strategy", "divergence", "recompute-status", "closeout", "step add", "step list", "step done", "step skip", "step link", "descendants"};
constexpr std::string_view k_leaves_question[]    = {"add", "edit", "view", "diff", "review", "answer", "wontfix", "list", "show", "link"};
constexpr std::string_view k_leaves_resume[]      = {"validate"};
constexpr std::string_view k_leaves_run[]         = {"start", "event", "finish", "show"};
constexpr std::string_view k_leaves_scenario[]    = {"add", "edit", "view", "diff", "review", "verify", "retire", "list", "show", "link"};
constexpr std::string_view k_leaves_scope[]       = {"show", "suggest", "use", "pop", "clear"};
constexpr std::string_view k_leaves_skills[]      = {"render", "status", "repair"};
constexpr std::string_view k_leaves_spec[]        = {"ingest"};
constexpr std::string_view k_leaves_sync[]        = {"pull", "push", "status", "resolve"};
constexpr std::string_view k_leaves_task[]        = {"add", "show", "list", "update", "edit", "view", "diff", "review", "done", "cancel", "block", "link", "reopen", "touches add", "touches list", "touches remove"};
constexpr std::string_view k_leaves_templates[]   = {"list", "show", "render", "validate", "init", "path"};
constexpr std::string_view k_leaves_test_spec[]   = {"status"};
constexpr std::string_view k_leaves_workbench[]   = {"pull", "push", "status", "resolve", "sync", "archive", "restore", "gc", "list", "publish", "extract-questions", "edit"};
constexpr std::string_view k_leaves_workflow[]    = {"list", "show", "run"};
constexpr std::string_view k_leaves_workspace[]   = {"init", "doctor", "routing build", "routing show", "regenerate"};

constexpr planar_verb_rule k_planar_verb_rules[] = {
    {"annotate", k_leaves_annotate},
    {"artifact", k_leaves_artifact},
    {"assoc", k_leaves_assoc},
    {"audit", k_leaves_audit},
    {"bench", k_leaves_bench},
    {"capture", k_leaves_capture},
    {"closure", k_leaves_closure},
    {"completion", k_leaves_empty},
    {"config", k_leaves_config},
    {"dashboard", k_leaves_empty},
    {"decision", k_leaves_decision},
    {"demote", k_leaves_empty},
    {"explore", k_leaves_empty},
    {"ext", k_leaves_ext},
    {"feedback", k_leaves_feedback},
    {"groups", k_leaves_groups},
    {"handoff", k_leaves_handoff},
    {"health", k_leaves_empty},
    {"import", k_leaves_empty},
    {"init", k_leaves_empty},
    {"link", k_leaves_empty},
    {"links", k_leaves_links},
    {"local", k_leaves_local},
    {"models", k_leaves_models},
    {"plan", k_leaves_plan},
    {"promote", k_leaves_empty},
    {"question", k_leaves_question},
    {"report", k_leaves_empty},
    {"resume", k_leaves_resume},
    {"run", k_leaves_run},
    {"scenario", k_leaves_scenario},
    {"schema", k_leaves_empty},
    {"scope", k_leaves_scope},
    {"search", k_leaves_empty},
    {"skills", k_leaves_skills},
    {"spec", k_leaves_spec},
    {"sync", k_leaves_sync},
    {"synthesize", k_leaves_empty},
    {"task", k_leaves_task},
    {"templates", k_leaves_templates},
    {"test-spec", k_leaves_test_spec},
    {"tree", k_leaves_empty},
    {"unlink", k_leaves_empty},
    {"version", k_leaves_empty},
    {"workbench", k_leaves_workbench},
    {"workflow", k_leaves_workflow},
    {"workspace", k_leaves_workspace},
};
// clang-format on

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

/// @brief Extract a canonical, bounded `planar ...` verb path from a
/// shell command string, or unset when it does not match the known verb
/// table exactly.
auto bounded_planar_verb_path(std::string_view command) -> std::optional<std::string_view> {
  auto const trim_ws = [](std::string_view s) {
    std::size_t b     = 0;
    std::size_t e     = s.size();
    auto const  is_ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (b < e && is_ws(s[b])) {
      ++b;
    }
    while (e > b && is_ws(s[e - 1])) {
      --e;
    }
    return s.substr(b, e - b);
  };
  std::string_view const trimmed = trim_ws(command);
  if (trimmed.find_first_of(";&|\n\r") != std::string_view::npos) {
    return std::nullopt;
  }
  auto const words = tokenize(trimmed);
  if (words.empty()) {
    return std::nullopt;
  }
  std::string_view const executable = words[0];
  if (executable != "planar") {
    return std::nullopt;
  }
  if (words.size() < 2) {
    return std::nullopt;
  }
  std::string_view const domain = words[1];
  auto const             base   = reinterpret_cast<std::uintptr_t>(trimmed.data());
  auto const domain_end = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(domain.data()) - base) + domain.size();

  for (auto const& rule : k_planar_verb_rules) {
    if (rule.domain != domain) {
      continue;
    }
    for (auto const& leaf : rule.leaves) {
      auto const  leaf_words      = tokenize(leaf);
      std::size_t candidate_index = 2; // words after `planar <domain>`
      std::size_t end             = domain_end;
      bool        matched         = true;
      for (auto const& expected : leaf_words) {
        if (candidate_index >= words.size()) {
          matched = false;
          break;
        }
        std::string_view const actual = words[candidate_index];
        if (actual != expected) {
          matched = false;
          break;
        }
        end = static_cast<std::size_t>(reinterpret_cast<std::uintptr_t>(actual.data()) - base) + actual.size();
        ++candidate_index;
      }
      if (matched) {
        std::string_view const path = trimmed.substr(0, end);
        return path.size() <= 96 ? std::optional<std::string_view>{path} : std::nullopt;
      }
    }
    return std::nullopt;
  }
  return std::nullopt;
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
  std::string_view verb_path;
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
  return extract_result{extract_kind::normalized, extracted{*command, *cat, *timestamp}};
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
      auto const verb_path = bounded_planar_verb_path(command);
      if (!verb_path.has_value()) {
        continue;
      }
      if (find_claude_tool(state.claude_tools, id) != nullptr) {
        continue;
      }
      if (state.claude_tools.size() == k_default_max_records) {
        continue;
      }
      state.claude_tools.push_back(pending_claude_tool{std::string{id}, std::string{*verb_path}, false});
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

auto extract_codex(const jd::json_value& obj) -> extract_result {
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
  if (*payload_type == "function_call") {
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
    if (!string_value(payload->find("output")).has_value()) {
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
    return extract_codex(value);
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
      .verb_path  = std::string{item.verb_path},
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

auto collect_preview(std::span<const raw_source> sources) -> preview {
  std::vector<signal_row>   signals;
  std::vector<coverage_row> coverage;

  for (auto const& source : sources) {
    extract_state state;
    coverage_row  cov{.v = source.v, .state = coverage_state::observed};
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
  // Correct fidelity call (the "raw vendor fixtures..." test's two `cli_log`
  // rows exercise exactly this tie and pin the resulting order), but no
  // test here flips `stable_sort` back to `sort` and re-asserts — so a
  // future regression to an unstable sort is not itself pinned, only this
  // one tie's OUTCOME is. Do not read the passing test as coverage for the
  // stability guarantee in general.
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

/// @brief Discover and read one vendor's transcript source, applying the
/// shared file/byte/record caps, and append exactly one `raw_source` to
/// `owned`. Mirrors the oracle's `collectVendorPath` (zig:307-416).
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
/// @param owned Sources collected so far; one row is appended.
/// @param warnings Extra warnings raised outside `collect_preview`'s own
/// coverage-driven set (file/byte/record caps, directory-open failures).
/// @param v Which vendor this call is for.
/// @param enabled Whether the vendor's adapter is configured on.
/// @param override_path The operator override path, or empty for the built-in.
/// @param home The operator's home directory.
/// @param builtin_rel The built-in path, relative to `home`.
/// @param jsonl_only When true (Claude, Codex), only `.jsonl`-suffixed
/// files count inside a directory; Copilot (`false`) takes every file.
/// @param files_left Remaining file budget, shared across all three vendors.
/// @param bytes_left Remaining byte budget, shared across all three vendors.
/// @param records_left Remaining record budget, shared across all three vendors.
/// @param fault The fault-injection seam (tests only).
auto collect_vendor_path(std::vector<raw_source>& owned, std::vector<warning_row>& warnings, vendor v, bool enabled,
                         std::string_view override_path, std::string_view home, std::string_view builtin_rel, bool jsonl_only,
                         std::size_t& files_left, std::size_t& bytes_left, std::size_t& records_left, const fs_fault& fault)
    -> void {
  if (!enabled) {
    owned.push_back(raw_source{.v = v, .enabled = false});
    return;
  }

  std::filesystem::path const builtin      = std::filesystem::path{std::string{home}} / builtin_rel;
  std::string                 selected_str = override_path.empty() ? builtin.string() : std::string{override_path};
  constexpr std::string_view  suffix{"/**/*.jsonl"};
  if (selected_str.size() >= suffix.size() && std::string_view{selected_str}.ends_with(suffix)) {
    selected_str.resize(selected_str.size() - suffix.size());
  }
  std::filesystem::path const selected{selected_str};

  if (fs_should_fail(fault, v, fs_operation::selected_stat, selected_str)) {
    owned.push_back(raw_source{.v = v, .available = false});
    return;
  }
  std::error_code status_ec;
  auto const      status = std::filesystem::status(selected, status_ec);
  if (status_ec) {
    owned.push_back(raw_source{.v = v, .available = false});
    return;
  }

  std::vector<std::string> paths;
  if (status.type() == std::filesystem::file_type::regular) {
    paths.push_back(selected_str);
  } else if (status.type() == std::filesystem::file_type::directory) {
    if (fs_should_fail(fault, v, fs_operation::directory_open, selected_str)) {
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
      owned.push_back(raw_source{.v = v, .available = false});
      return;
    }
    std::error_code                           open_ec;
    std::filesystem::directory_iterator const probe(selected, open_ec);
    if (open_ec) {
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
      owned.push_back(raw_source{.v = v, .available = false});
      return;
    }
    if (fs_should_fail(fault, v, fs_operation::directory_walk, selected_str)) {
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
      owned.push_back(raw_source{.v = v, .available = false});
      return;
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
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
      owned.push_back(raw_source{.v = v, .available = false});
      return;
    }
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
        warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
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
        warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
        break;
      }
    }
  } else {
    owned.push_back(raw_source{.v = v, .available = false});
    return;
  }

  std::ranges::sort(paths);

  std::string combined;
  std::size_t scanned_files = 0;
  std::size_t io_failures   = 0;
  for (auto const& path : paths) {
    if (files_left == 0) {
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::file_cap});
      break;
    }
    if (fs_should_fail(fault, v, fs_operation::file_stat, path)) {
      ++io_failures;
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
      continue;
    }
    std::error_code size_ec;
    auto const      size = std::filesystem::file_size(path, size_ec);
    if (size_ec) {
      ++io_failures;
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
      continue;
    }
    if (size > bytes_left) {
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::byte_cap});
      break;
    }
    if (fs_should_fail(fault, v, fs_operation::file_read, path)) {
      ++io_failures;
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
      continue;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      ++io_failures;
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::unavailable});
      continue;
    }
    std::string const bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    auto const        record_count = count_records(bytes);
    if (record_count > records_left) {
      warnings.push_back(warning_row{.v = v, .kind = warning_kind::record_cap});
      break;
    }
    combined += bytes;
    combined += '\n';
    files_left -= 1;
    bytes_left -= bytes.size();
    records_left -= record_count;
    ++scanned_files;
  }

  if (scanned_files == 0 && io_failures != 0) {
    owned.push_back(raw_source{.v = v, .available = false});
  } else {
    owned.push_back(raw_source{.v = v, .jsonl = std::move(combined)});
  }
}

} // namespace

auto collect_preview_from_paths(const transcript_config& config, const std::optional<cli_log_adapter>& cli,
                                const collector_limits& limits, const fs_fault& fault) -> preview {
  std::vector<raw_source>  owned;
  std::vector<warning_row> extra_warnings;
  std::size_t              files_left   = limits.max_files;
  std::size_t              bytes_left   = limits.max_bytes;
  std::size_t              records_left = limits.max_records;

  collect_vendor_path(owned, extra_warnings, vendor::claude, config.claude_enabled, config.claude_path, config.home_dir,
                      ".claude/projects", true, files_left, bytes_left, records_left, fault);
  collect_vendor_path(owned, extra_warnings, vendor::codex, config.codex_enabled, config.codex_path, config.home_dir,
                      ".codex/sessions", true, files_left, bytes_left, records_left, fault);
  collect_vendor_path(owned, extra_warnings, vendor::copilot, config.copilot_enabled, config.copilot_path, config.home_dir,
                      ".copilot/session-state", false, files_left, bytes_left, records_left, fault);

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

  auto result = collect_preview(owned);
  if (!extra_warnings.empty()) {
    result.warnings.insert(result.warnings.end(), std::make_move_iterator(extra_warnings.begin()),
                           std::make_move_iterator(extra_warnings.end()));
  }
  return result;
}

} // namespace planar::engine::introspection_adapters
