/// @file importer.cpp
/// @brief Implementation of the filesystem-only import staging engine.
module planar.engine.importer;

import std;
import planar.json_text;
import planar.json_dom;
import planar.sha256;

namespace planar::engine::importer {
namespace {
auto slugify(std::string_view raw) -> std::string {
  std::string out;
  bool        dash = false;
  for (unsigned char c : raw) {
    if (std::isalnum(c)) {
      out.push_back(static_cast<char>(std::tolower(c)));
      dash = false;
    } else if (!out.empty() && !dash) {
      out.push_back('-');
      dash = true;
    }
  }
  while (!out.empty() && out.back() == '-')
    out.pop_back();
  return out.empty() ? "repo" : out;
}
auto first_heading(const std::filesystem::path& root, std::string_view fallback) -> std::string {
  std::ifstream in(root / "README.md", std::ios::binary);
  std::string   line;
  while (std::getline(in, line)) {
    auto const start = line.find_first_not_of(" \t\r");
    if (start != std::string::npos && line[start] == '#') {
      auto pos = line.find_first_not_of("# \t", start);
      if (pos != std::string::npos)
        return line.substr(pos);
    }
  }
  return std::string{fallback};
}
auto write_atomic(const std::filesystem::path& path, std::string_view body) -> bool {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec)
    return false;
  auto const temp = path.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out)
      return false;
    out << body;
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

// The interpretation cache is an untrusted hand-off boundary: it is written
// by a vendor skill, not by this process.  Do the envelope validation here,
// before a layer-3 caller is allowed to turn its contents into rows.  In
// particular, accepting a cache for another fingerprint would silently apply
// a result produced from a different repository snapshot.
auto cache_anchor(std::string_view body, const request& req) -> std::optional<std::string> {
  auto parsed = json_dom::parse_json(body);
  if (!parsed || parsed->kind != json_dom::json_kind::object)
    return std::nullopt;
  auto const* version       = parsed->find("schema_version");
  auto const* fingerprint   = parsed->find("fingerprint");
  auto const* title         = parsed->find("anchor_title");
  auto const* provenance    = parsed->find("provenance");
  auto const* phases        = parsed->find("phases");
  auto const* forward_specs = parsed->find("forward_specs");
  if (version == nullptr || version->kind != json_dom::json_kind::integer || version->integer != 1 || fingerprint == nullptr ||
      fingerprint->kind != json_dom::json_kind::string || fingerprint->string != req.fingerprint || title == nullptr ||
      title->kind != json_dom::json_kind::string || title->string.empty() || provenance == nullptr ||
      provenance->kind != json_dom::json_kind::string || provenance->string.empty() || phases == nullptr ||
      phases->kind != json_dom::json_kind::array || forward_specs == nullptr ||
      forward_specs->kind != json_dom::json_kind::array || forward_specs->array.size() < 3 || forward_specs->array.size() > 5)
    return std::nullopt;
  // Unique, non-empty phase and forward-spec slugs are the two identity sets
  // the reconciliation pass keys on. Reject malformed caches early instead
  // of letting a later apply coalesce unrelated rows.
  std::set<std::string> phase_slugs;
  std::set<std::string> spec_slugs;
  for (auto const& phase : phases->array) {
    auto const* slug   = phase.find("slug");
    auto const* status = phase.find("status");
    auto const* tasks  = phase.find("tasks");
    if (phase.kind != json_dom::json_kind::object || slug == nullptr || slug->kind != json_dom::json_kind::string ||
        slug->string.empty() || status == nullptr || status->kind != json_dom::json_kind::string || tasks == nullptr ||
        tasks->kind != json_dom::json_kind::array || !phase_slugs.insert(slug->string).second)
      return std::nullopt;
    std::set<std::string> task_slugs;
    for (auto const& task : tasks->array) {
      auto const* task_slug = task.find("slug");
      if (task.kind != json_dom::json_kind::object || task_slug == nullptr || task_slug->kind != json_dom::json_kind::string ||
          task_slug->string.empty() || !task_slugs.insert(task_slug->string).second)
        return std::nullopt;
    }
  }
  for (auto const& spec : forward_specs->array) {
    auto const* slug = spec.find("slug");
    if (spec.kind != json_dom::json_kind::object || slug == nullptr || slug->kind != json_dom::json_kind::string ||
        slug->string.empty() || !spec_slugs.insert(slug->string).second)
      return std::nullopt;
  }
  return title->string;
}
} // namespace

auto run(const std::filesystem::path& root, const std::filesystem::path& planar_home, bool interpret)
    -> std::expected<outcome, error> {
  std::error_code ec;
  if (root.empty())
    return std::unexpected(error::invalid_input);
  auto const canonical = std::filesystem::canonical(root, ec);
  if (ec || !std::filesystem::is_directory(canonical, ec))
    return std::unexpected(error::not_found);
  request req;
  req.repo_root    = canonical.string();
  req.repo_slug    = slugify(canonical.filename().string());
  req.anchor_title = first_heading(canonical, req.repo_slug);
  std::vector<std::pair<std::string, std::string>> docs, guides;
  std::vector<std::string>                         tree;
  for (std::filesystem::recursive_directory_iterator it(canonical, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_directory(ec) || ec)
      continue;
    ++req.tree_count;
    auto const name     = it->path().filename().string();
    auto const ext      = it->path().extension().string();
    auto       relative = std::filesystem::relative(it->path(), canonical, ec).generic_string();
    if (ec)
      return std::unexpected(error::io);
    std::ifstream in(it->path(), std::ios::binary);
    std::string   body{std::istreambuf_iterator<char>{in}, {}};
    if (name == "AGENTS.md" || name == "CLAUDE.md") {
      ++req.guide_count;
      guides.emplace_back(relative, std::move(body));
    } else if (ext == ".md") {
      ++req.docs_count;
      docs.emplace_back(relative, std::move(body));
    }
  }
  if (ec)
    return std::unexpected(error::io);
  for (auto const& e : std::filesystem::directory_iterator(canonical, ec))
    tree.push_back(e.path().filename().generic_string() + (e.is_directory() ? "/" : ""));
  if (ec)
    return std::unexpected(error::io);
  std::ranges::sort(docs);
  std::ranges::sort(guides);
  std::ranges::sort(tree);
  std::string fingerprint_input;
  auto        rec = [&](std::string_view a, std::string_view b) {
    fingerprint_input.append(a);
    fingerprint_input.push_back('\0');
    fingerprint_input.append(b);
    fingerprint_input.push_back('\0');
  };
  rec("repo_slug", req.repo_slug);
  rec("readme", first_heading(canonical, ""));
  rec("docs", std::to_string(docs.size()));
  for (auto const& [p, b] : docs)
    rec(p, b);
  rec("guide_files", std::to_string(guides.size()));
  for (auto const& [p, b] : guides)
    rec(p, b);
  rec("git_log", "0");
  rec("tree_summary", std::to_string(tree.size()));
  for (auto const& e : tree) {
    fingerprint_input.append(e);
    fingerprint_input.push_back('\0');
  }
  rec("detected_artifacts", std::to_string(docs.size()));
  for (auto const& [p, b] : docs) {
    auto n    = std::filesystem::path(p).filename().string();
    auto kind = n.contains("roadmap") ? "roadmap" : n.contains("tech") ? "tech_spec" : n == "README.md" ? "readme" : "research";
    rec(p, kind);
    rec("source", "filename");
  }
  req.fingerprint = sha256::hex(fingerprint_input);
  outcome out{.request_ = req};
  if (!interpret) {
    out.message = "import: interpretation disabled; run with --interpret to stage/consume LLM artifacts.";
    return out;
  }
  out.cache_path   = planar_home / "llm" / "import-interpretation" / req.repo_slug / (req.fingerprint + ".json");
  out.pending_path = planar_home / "llm" / "pending" / "import-interpretation" / (req.repo_slug + ".json");
  if (std::filesystem::exists(out.cache_path, ec) && !ec) {
    std::ifstream cached(out.cache_path, std::ios::binary);
    std::string   cache_body{std::istreambuf_iterator<char>{cached}, std::istreambuf_iterator<char>{}};
    auto          anchor = cache_anchor(cache_body, req);
    if (!anchor)
      return std::unexpected(error::invalid_input);
    out.mode_                    = outcome::mode::cache_hit;
    out.interpreted_anchor_title = std::move(*anchor);
    out.message                  = std::format("Loaded cached interpretation result: {}", out.cache_path.string());
    return out;
  }
  std::string body = "{\"schema_version\":1,\"repo_slug\":" + json_text::json_string(req.repo_slug) +
                     ",\"repo_root\":" + json_text::json_string(req.repo_root) +
                     ",\"fingerprint\":" + json_text::json_string(req.fingerprint) + "}\n";
  if (!write_atomic(out.pending_path, body))
    return std::unexpected(error::io);
  out.mode_   = outcome::mode::pending;
  out.message = std::format("Awaiting LLM interpretation. The vendor skill should:\n  1. read  {}\n  2. run the LLM at "
                            "temperature 0\n  3. write the Result to {}\n  4. re-invoke `planar import <repo> --interpret`\nSee "
                            "`commands/claude/pl-import.md` for the full contract.",
                            out.pending_path.string(), out.cache_path.string());
  return out;
}
} // namespace planar::engine::importer
