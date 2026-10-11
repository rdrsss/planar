/// @file synthesize.cpp
/// @brief Filesystem/cache implementation of the synthesis handoff protocol.
module planar.engine.synthesize;

import std;
import planar.json_text;
import planar.json_dom;
import planar.sha256;

namespace planar::engine::synthesize {
namespace {

auto lower(std::string value) -> std::string {
  std::ranges::transform(value, value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}
auto is_source(std::string_view name) -> bool {
  auto const n = lower(std::string{name});
  for (auto const suffix :
       {".go", ".zig", ".ts", ".tsx", ".js", ".jsx", ".py", ".swift", ".rs", ".c", ".cc", ".cpp", ".h", ".hpp"})
    if (n.ends_with(suffix))
      return true;
  return false;
}
auto is_test(std::string_view name, std::string_view rel) -> bool {
  auto const n = lower(std::string{name});
  return n.ends_with("_test.go") || n.ends_with(".test.ts") || n.ends_with(".spec.ts") || n.ends_with("_test.py") ||
         n.ends_with("tests.swift") || rel.contains("/test/") || rel.contains("/tests/");
}
auto read_file(const std::filesystem::path& path, std::size_t limit = 1024 * 1024) -> std::optional<std::string> {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return std::nullopt;
  std::string out{std::istreambuf_iterator<char>{in}, {}};
  if (out.size() > limit)
    return std::nullopt;
  return out;
}
auto area_key(std::string_view rel) -> std::string {
  auto first = rel.find('/');
  if (first == std::string_view::npos) {
    auto dot = rel.find('.');
    return std::string{rel.substr(0, dot)};
  }
  auto second = rel.find('/', first + 1);
  return std::string{rel.substr(0, second)};
}
auto count_lines(std::string_view bytes) -> std::int64_t {
  if (bytes.empty())
    return 0;
  return 1 + static_cast<std::int64_t>(std::ranges::count(bytes, '\n'));
}
auto valid_layout(std::string_view value) -> bool {
  return value.empty() || value == "swift" || value == "go" || value == "node" || value == "python" || value == "mixed";
}
auto parse_provider(std::optional<std::string> raw) -> std::optional<provider> {
  if (!raw || raw->empty() || *raw == "shell")
    return provider::shell;
  if (*raw == "anthropic")
    return provider::anthropic;
  if (*raw == "openai")
    return provider::openai;
  return std::nullopt;
}
auto write_atomic(const std::filesystem::path& path, std::string_view body) -> bool {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec)
    return false;
  auto const temp = std::filesystem::path(path.string() + ".tmp");
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out)
      return false;
    out.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!out)
      return false;
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(temp, path, ec);
  }
  return !ec;
}
auto string_member(const json_dom::json_value& value, std::string_view key) -> const std::string* {
  auto const* member = value.find(key);
  return member != nullptr && member->kind == json_dom::json_kind::string ? &member->string : nullptr;
}
auto integer_member(const json_dom::json_value& value, std::string_view key) -> const std::int64_t* {
  auto const* member = value.find(key);
  return member != nullptr && member->kind == json_dom::json_kind::integer ? &member->integer : nullptr;
}
auto boolean_member(const json_dom::json_value& value, std::string_view key) -> const bool* {
  auto const* member = value.find(key);
  return member != nullptr && member->kind == json_dom::json_kind::boolean ? &member->boolean : nullptr;
}
auto valid_plan_status(std::string_view value) -> bool {
  return value == "draft" || value == "active" || value == "paused" || value == "done" || value == "abandoned";
}
auto valid_task_status(std::string_view value) -> bool {
  return value == "todo" || value == "doing" || value == "blocked" || value == "done" || value == "cancelled";
}
auto valid_doc_path(const request& req, std::string_view rel) -> bool {
  return rel.empty() || std::filesystem::exists(std::filesystem::path(req.repo_root) / rel);
}
auto evidence_path_allowed(const request& req, std::string_view rel) -> bool {
  return std::ranges::any_of(req.areas, [&](auto const& a) { return rel == a.path || rel.starts_with(a.path + "/"); });
}
auto validate_result(const json_dom::json_value& value, const request& req) -> bool {
  if (value.kind != json_dom::json_kind::object)
    return false;
  auto const* version     = integer_member(value, "schema_version");
  auto const* fp          = string_member(value, "fingerprint");
  auto const* synthesized = boolean_member(value, "synthesized");
  auto const* title       = string_member(value, "anchor_title");
  auto const* provenance  = string_member(value, "provenance");
  auto const* phases      = value.find("phases");
  auto const* decisions   = value.find("decisions");
  auto const* deferred    = value.find("deferred_items");
  auto const* specs       = value.find("forward_specs");
  auto const* refs        = value.find("reference_artifacts");
  if (!version || *version != 1 || !fp || *fp != req.fingerprint || !synthesized || !*synthesized || !title || title->empty() ||
      !provenance || provenance->empty() || (phases && phases->kind != json_dom::json_kind::array) ||
      (decisions && decisions->kind != json_dom::json_kind::array) ||
      (deferred && deferred->kind != json_dom::json_kind::array) || !specs || specs->kind != json_dom::json_kind::array ||
      specs->array.size() < 3 || specs->array.size() > 5 || (refs && refs->kind != json_dom::json_kind::array))
    return false;
  std::set<std::string> phase_slugs;
  if (phases)
    for (auto const& phase : phases->array) {
      auto const* slug   = string_member(phase, "slug");
      auto const* status = string_member(phase, "status");
      auto const* tasks  = phase.find("tasks");
      if (phase.kind != json_dom::json_kind::object || !slug || !phase_slugs.insert(*slug).second || !status ||
          !valid_plan_status(*status) || (tasks && tasks->kind != json_dom::json_kind::array))
        return false;
      std::set<std::string> task_slugs;
      std::size_t           doing = 0;
      if (tasks)
        for (auto const& task : tasks->array) {
          auto const* task_slug   = string_member(task, "slug");
          auto const* task_status = string_member(task, "status");
          auto const* priority    = integer_member(task, "priority");
          auto const* citations   = task.find("citations");
          auto const* code        = task.find("code_evidence");
          if (task.kind != json_dom::json_kind::object || !task_slug || !task_slugs.insert(*task_slug).second || !task_status ||
              !valid_task_status(*task_status) || (priority && (*priority < 0 || *priority > 1000)) ||
              (citations && citations->kind != json_dom::json_kind::array) || (code && code->kind != json_dom::json_kind::array))
            return false;
          doing += *task_status == "doing";
          if (citations)
            for (auto const& citation : citations->array) {
              auto const* path = string_member(citation, "path");
              if (!path || !valid_doc_path(req, *path))
                return false;
            }
          if (*task_status != "todo") {
            if (req.greenfield || !code || code->array.empty())
              return false;
            for (auto const& citation : code->array) {
              auto const* path = string_member(citation, "path");
              if (!path || !evidence_path_allowed(req, *path))
                return false;
            }
          }
        }
      if (doing > 1)
        return false;
    }
  if (decisions)
    for (auto const& decision : decisions->array) {
      auto const* source   = string_member(decision, "source");
      auto const* citation = decision.find("citation");
      if (!source || (*source != "tech-spec" && *source != "llm-inferred") || !citation ||
          citation->kind != json_dom::json_kind::object)
        return false;
      auto const* path = string_member(*citation, "path");
      if (*source == "llm-inferred" && (!path || path->empty()))
        return false;
      if (path && !path->empty() && !valid_doc_path(req, *path))
        return false;
    }
  if (deferred)
    for (auto const& item : deferred->array) {
      auto const* priority = integer_member(item, "priority");
      auto const* phase    = string_member(item, "phase_slug");
      if (!priority || *priority < 150 || !phase || !phase_slugs.contains(*phase))
        return false;
    }
  std::set<std::string> spec_slugs;
  for (auto const& spec : specs->array) {
    auto const* slug = string_member(spec, "slug");
    if (!slug || slug->empty() || !spec_slugs.insert(*slug).second)
      return false;
  }
  if (refs)
    for (auto const& ref : refs->array) {
      auto const* path = string_member(ref, "path");
      if (!path || path->empty() || !valid_doc_path(req, *path))
        return false;
    }
  return true;
}
auto append_record(std::string& out, std::initializer_list<std::string_view> parts) -> void {
  for (auto part : parts) {
    out.append(part);
    out.push_back('\0');
  }
}
auto encode_request(const request& req) -> std::string {
  std::string out =
      std::format("{{\"schema_version\":1,\"repo_slug\":{},\"repo_root\":{},\"fingerprint\":{},\"readme\":{},\"docs\":{{",
                  json_text::json_string(req.repo_slug), json_text::json_string(req.repo_root),
                  json_text::json_string(req.fingerprint), json_text::json_string(req.readme));
  for (std::size_t i = 0; i < req.docs.size(); ++i) {
    if (i)
      out.push_back(',');
    out += json_text::json_string(req.docs[i].first) + ":" + json_text::json_string(req.docs[i].second);
  }
  out += "},\"guide_files\":{";
  for (std::size_t i = 0; i < req.guide_files.size(); ++i) {
    if (i)
      out.push_back(',');
    out += json_text::json_string(req.guide_files[i].first) + ":" + json_text::json_string(req.guide_files[i].second);
  }
  out += "},\"git_log\":[],\"tree_summary\":[";
  for (std::size_t i = 0; i < req.tree_summary.size(); ++i) {
    if (i)
      out.push_back(',');
    out += json_text::json_string(req.tree_summary[i]);
  }
  out += std::format("],\"code_evidence\":{{\"layout\":{},\"areas\":[", json_text::json_string(req.layout));
  for (std::size_t i = 0; i < req.areas.size(); ++i) {
    if (i)
      out.push_back(',');
    auto const& a = req.areas[i];
    out += std::format("{{\"name\":{},\"path\":{},\"source_files\":{},\"test_files\":{},\"first_commit\":null,\"last_commit\":"
                       "null,\"commit_count\":0,\"signal_strength\":{}}}",
                       json_text::json_string(a.name), json_text::json_string(a.path), a.source_files, a.test_files,
                       a.signal_strength);
  }
  out += std::format("],\"total_files\":{},\"total_lines\":{},\"has_tests\":{},\"has_ci\":{},\"recent_commits\":[]}},"
                     "\"greenfield\":{},\"code_layout\":{}}}\n",
                     req.total_files, req.total_lines, req.has_tests, req.has_ci, req.greenfield,
                     json_text::json_string(req.code_layout));
  return out;
}
auto build_request(const std::filesystem::path& canonical, const std::filesystem::path& display_root, const options& opts)
    -> std::expected<request, error> {
  request req;
  req.repo_root   = display_root.string();
  req.repo_slug   = lower(display_root.filename().string().empty() ? "repo" : display_root.filename().string());
  req.code_layout = opts.code_layout.value_or("");
  std::map<std::string, std::pair<std::int64_t, std::int64_t>, std::less<>> counters;
  std::error_code                                                           ec;
  for (std::filesystem::recursive_directory_iterator it(canonical, ec), end; !ec && it != end; it.increment(ec)) {
    auto const name = it->path().filename().string();
    if (it->is_directory(ec)) {
      if (name == ".git" || (!name.empty() && name.front() == '.'))
        it.disable_recursion_pending();
      continue;
    }
    if (!it->is_regular_file(ec))
      continue;
    auto rel = std::filesystem::relative(it->path(), canonical, ec).generic_string();
    if (ec)
      return std::unexpected(error::io);
    auto const lname = lower(name);
    bool const guide = lname == "agents.md" || lname == "claude.md";
    if (guide || std::filesystem::path(lname).extension() == ".md") {
      auto body = read_file(it->path());
      if (!body)
        return std::unexpected(error::io);
      (guide ? req.guide_files : req.docs).emplace_back(rel, std::move(*body));
    }
    if (is_source(name)) {
      auto body = read_file(it->path(), 512 * 1024);
      if (!body)
        return std::unexpected(error::io);
      auto& count = counters[area_key(rel)];
      if (is_test(name, rel)) {
        ++count.second;
        req.has_tests = true;
      } else
        ++count.first;
      ++req.total_files;
      req.total_lines += count_lines(*body);
    }
  }
  if (ec)
    return std::unexpected(error::io);
  std::ranges::sort(req.docs);
  std::ranges::sort(req.guide_files);
  for (auto const& [path, body] : req.docs) {
    auto const base = lower(std::filesystem::path(path).filename().string());
    if (base == "readme" || base == "readme.md") {
      req.readme = body;
      break;
    }
  }
  for (std::filesystem::directory_iterator it(canonical, ec), end; !ec && it != end; it.increment(ec)) {
    auto const name = it->path().filename().string();
    if (name == ".git")
      continue;
    req.tree_summary.push_back(name + (it->is_directory(ec) ? "/" : ""));
  }
  if (ec)
    return std::unexpected(error::io);
  std::ranges::sort(req.tree_summary);
  if (std::filesystem::exists(canonical / "go.mod"))
    req.layout = "go";
  else if (std::filesystem::exists(canonical / "package.json"))
    req.layout = "node";
  else if (std::filesystem::exists(canonical / "pyproject.toml") || std::filesystem::exists(canonical / "requirements.txt"))
    req.layout = "python";
  else if (std::filesystem::exists(canonical / "Package.swift"))
    req.layout = "swift";
  else
    req.layout = "mixed";
  req.has_ci = std::filesystem::exists(canonical / ".github/workflows") ||
               std::filesystem::exists(canonical / ".gitlab-ci.yml") ||
               std::filesystem::exists(canonical / ".circleci/config.yml");
  for (auto const& [path, counts] : counters) {
    auto const slash = path.find_last_of('/');
    req.areas.push_back({.name            = path.substr(slash == std::string::npos ? 0 : slash + 1),
                         .path            = path,
                         .source_files    = counts.first,
                         .test_files      = counts.second,
                         .signal_strength = counts.first == 0   ? 0.0
                                            : counts.second > 0 ? (req.has_tests ? 1.0 : 0.7)
                                                                : 0.2});
  }
  // Auto-greenfield fires either when the repo is literally empty, or when
  // every detected code area carries zero signal strength (a tests-only or
  // otherwise substance-free tree still has files/lines, but none of them
  // constitute real production evidence). Mirrors the Zig oracle's
  // isAutoGreenfield (task 6453): a bare zero-files check missed the latter
  // case entirely.
  auto const all_areas_zero_signal = std::ranges::all_of(req.areas, [](auto const& area) { return area.signal_strength == 0.0; });
  req.greenfield = opts.treat_as_greenfield ||
                   (!opts.treat_as_nongreenfield && (req.total_files == 0 || req.total_lines == 0 || all_areas_zero_signal));
  std::string input;
  append_record(input, {"repo_slug", req.repo_slug});
  append_record(input, {"readme", req.readme});
  append_record(input, {"docs", std::to_string(req.docs.size())});
  for (auto const& [path, body] : req.docs)
    append_record(input, {path, body});
  append_record(input, {"guide_files", std::to_string(req.guide_files.size())});
  for (auto const& [path, body] : req.guide_files)
    append_record(input, {path, body});
  append_record(input, {"tree_summary", std::to_string(req.tree_summary.size())});
  for (auto const& entry : req.tree_summary)
    append_record(input, {entry});
  append_record(input, {"git_log", "0"});
  append_record(input, {"layout", req.layout, req.code_layout});
  append_record(input, {"areas", std::to_string(req.areas.size())});
  for (auto const& a : req.areas) {
    append_record(input, {a.path, a.name});
    append_record(input, {"source_files", std::to_string(a.source_files)});
    append_record(input, {"test_files", std::to_string(a.test_files)});
  }
  append_record(input, {"greenfield", req.greenfield ? "1" : "0"});
  req.fingerprint = sha256::hex(input);
  return req;
}
} // namespace

auto provider_name(provider value) -> std::string_view {
  switch (value) {
  case provider::shell:
    return "shell";
  case provider::anthropic:
    return "anthropic";
  case provider::openai:
    return "openai";
  }
  return "shell";
}

auto run(const std::filesystem::path& root, const std::filesystem::path& planar_home, const options& opts, const env_lookup& env)
    -> std::expected<outcome, error> {
  if (opts.treat_as_greenfield && opts.treat_as_nongreenfield)
    return std::unexpected(error::invalid_input);
  if (opts.code_layout && !valid_layout(*opts.code_layout))
    return std::unexpected(error::invalid_input);
  std::error_code ec;
  auto            canonical = std::filesystem::canonical(root, ec);
  if (ec || !std::filesystem::is_directory(canonical, ec))
    return std::unexpected(error::not_found);
  // Report the caller's own spelling of `root`, not the symlink-resolved
  // `canonical` -- a shell keeps a symlink-spelled PWD, and repo_root/
  // repo_slug must key off that spelling, not realpath(3) (task 6453,
  // plan 351 task 2378). `canonical` remains the walk root: iterating the
  // resolved directory is correct either way.
  auto display_root = root.is_absolute() ? root.lexically_normal() : canonical;
  if (display_root.filename().empty())
    display_root = display_root.parent_path();
  auto raw_provider = opts.provider_override;
  if (!raw_provider)
    raw_provider = env("PLANAR_LLM_PROVIDER");
  auto kind = parse_provider(raw_provider);
  if (!kind)
    return std::unexpected(error::invalid_input);
  auto req = build_request(canonical, display_root, opts);
  if (!req)
    return std::unexpected(req.error());
  outcome out{.provider_ = *kind, .request_ = std::move(*req)};
  out.cache_path = planar_home / "cache" / "bootstrap-synthesis" / out.request_.repo_slug / (out.request_.fingerprint + ".json");
  out.pending_path = planar_home / "cache" / "bootstrap-synthesis" / out.request_.repo_slug / "_pending.json";
  if (std::filesystem::exists(out.cache_path, ec) && !ec) {
    auto raw = read_file(out.cache_path, 16 * 1024 * 1024);
    if (!raw)
      return std::unexpected(error::io);
    auto parsed = json_dom::parse_json(*raw);
    if (!parsed || !validate_result(*parsed, out.request_))
      return std::unexpected(error::invalid_input);
    out.mode_   = mode::cache_hit;
    out.result  = std::move(*parsed);
    out.message = std::format("Loaded cached synthesis result: {}", out.cache_path.string());
    return out;
  }
  if (opts.apply)
    return std::unexpected(error::not_found);
  // `--dry-run` (task 6273, decision 1125). There are no PLANNING writes on
  // this path to suppress -- a non-apply run creates zero plans and zero
  // tasks either way. Staging `_pending.json` (and the cache directory
  // holding it) is the entire side effect, so it is the entire thing the
  // flag suppresses. It reports the path it WOULD have written rather than
  // merely going quiet: an operator reaches for --dry-run when unsure what
  // a command does, and an answer of "nothing happened" tells them nothing.
  if (opts.dry_run) {
    out.message = std::format("dry run: nothing staged. Would have written the pending synthesis request to {}, and would read "
                              "the LLM's result back from {}.",
                              out.pending_path.string(), out.cache_path.string());
    return out;
  }
  if (!write_atomic(out.pending_path, encode_request(out.request_)))
    return std::unexpected(error::io);
  out.message = std::format(
      "Awaiting LLM synthesis. The vendor skill should:\n  1. read  {}\n  2. run the LLM at temperature 0\n  3. write the Result "
      "to {}\n  4. re-invoke `planar synthesize <repo-root>`\nSee `planar synthesize --help` for the full contract.",
      out.pending_path.string(), out.cache_path.string());
  return out;
}

} // namespace planar::engine::synthesize
