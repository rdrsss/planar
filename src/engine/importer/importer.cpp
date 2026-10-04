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

/// @brief A string member, or null when absent or not a string.
/// @param value The object to read.
/// @param key The member name.
/// @return Pointer to the stored string, or `nullptr`.
auto string_member(const json_dom::json_value& value, std::string_view key) -> const std::string* {
  auto const* member = value.find(key);
  return member != nullptr && member->kind == json_dom::json_kind::string ? &member->string : nullptr;
}

/// @brief An integer member, or null when absent or not an integer.
/// @param value The object to read.
/// @param key The member name.
/// @return Pointer to the stored integer, or `nullptr`.
auto integer_member(const json_dom::json_value& value, std::string_view key) -> const std::int64_t* {
  auto const* member = value.find(key);
  return member != nullptr && member->kind == json_dom::json_kind::integer ? &member->integer : nullptr;
}

/// @brief The five statuses a cached PHASE proposal may carry.
/// @param value The candidate status.
/// @return True when it is one Planar recognises.
auto valid_plan_status(std::string_view value) -> bool {
  return value == "draft" || value == "active" || value == "paused" || value == "done" || value == "abandoned";
}

/// @brief The five statuses a cached task proposal may carry.
/// @param value The candidate status.
/// @return True when it is one Planar recognises.
auto valid_task_status(std::string_view value) -> bool {
  return value == "todo" || value == "doing" || value == "blocked" || value == "done" || value == "cancelled";
}

/// @brief Whether a cited documentation path exists under the repo root.
///
/// An empty path is vacuously fine; a NAMED one must be there, because the
/// cache is asserting provenance the apply pass will record.
/// @param req The staged request, carrying the repo root.
/// @param rel A repo-relative path.
/// @return True when the path is empty or exists.
auto valid_doc_path(const request& req, std::string_view rel) -> bool {
  return rel.empty() || std::filesystem::exists(std::filesystem::path(req.repo_root) / rel);
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
        slug->string.empty() || status == nullptr || status->kind != json_dom::json_kind::string ||
        // Any non-empty string used to pass. The contract names exactly five.
        !valid_plan_status(status->string) || tasks == nullptr || tasks->kind != json_dom::json_kind::array ||
        !phase_slugs.insert(slug->string).second)
      return std::nullopt;
    std::set<std::string> task_slugs;
    // `doing` is a per-phase exclusivity rule, not a per-cache one: a phase
    // proposing two in-flight tasks is describing a state the planning model
    // does not allow, so reject it here rather than materializing it.
    std::size_t doing = 0;
    for (auto const& task : tasks->array) {
      auto const* task_slug   = task.find("slug");
      auto const* task_status = string_member(task, "status");
      auto const* priority    = integer_member(task, "priority");
      if (task.kind != json_dom::json_kind::object || task_slug == nullptr || task_slug->kind != json_dom::json_kind::string ||
          task_slug->string.empty() || !task_slugs.insert(task_slug->string).second || task_status == nullptr ||
          !valid_task_status(*task_status) ||
          // Priority is OPTIONAL, but a present one must be in range. The
          // port did not parse it at all, so an out-of-range value was
          // accepted and then written.
          (priority != nullptr && (*priority < 0 || *priority > 1000)))
        return std::nullopt;
      doing += static_cast<std::size_t>(*task_status == "doing");
    }
    if (doing > 1)
      return std::nullopt;
  }
  for (auto const& spec : forward_specs->array) {
    auto const* slug = spec.find("slug");
    if (spec.kind != json_dom::json_kind::object || slug == nullptr || slug->kind != json_dom::json_kind::string ||
        slug->string.empty() || !spec_slugs.insert(slug->string).second)
      return std::nullopt;
  }

  // Decisions carry PROVENANCE the apply pass records as fact. The contract
  // is `skills/src/pl-import.md` § "Hard contract rules", which requires a
  // citation only of an `llm-inferred` decision -- `source` itself is
  // OPTIONAL. The sibling `synthesize` validator demands both unconditionally
  // and is NOT the contract here: transcribing it verbatim rejected the
  // vendor skill's own documented output, which is how this was caught.
  if (auto const* decisions = parsed->find("decisions"); decisions != nullptr) {
    if (decisions->kind != json_dom::json_kind::array)
      return std::nullopt;
    for (auto const& decision : decisions->array) {
      if (decision.kind != json_dom::json_kind::object)
        return std::nullopt;
      auto const* source = string_member(decision, "source");
      // A source the cache DOES name must be one Planar understands; an
      // unnamed one is the deterministic-import case and carries no claim.
      if (source != nullptr && *source != "tech-spec" && *source != "llm-inferred")
        return std::nullopt;
      auto const* citation = decision.find("citation");
      if (citation != nullptr && citation->kind != json_dom::json_kind::object)
        return std::nullopt;
      auto const* path = citation != nullptr ? string_member(*citation, "path") : nullptr;
      // "Each decision with source: llm-inferred carries a non-empty
      // citation.path" -- the one unconditional requirement in the contract.
      if (source != nullptr && *source == "llm-inferred" && (path == nullptr || path->empty()))
        return std::nullopt;
      if (path != nullptr && !path->empty() && !valid_doc_path(req, *path))
        return std::nullopt;
    }
  }

  // `deferred_items` is VALIDATE-ONLY: nothing materializes it. It is checked
  // anyway because the port previously did not parse it at all, so a cache
  // carrying a malformed deferral was silently ACCEPTED -- a divergence on a
  // verb that writes planning entities.
  if (auto const* deferred = parsed->find("deferred_items"); deferred != nullptr) {
    if (deferred->kind != json_dom::json_kind::array)
      return std::nullopt;
    for (auto const& item : deferred->array) {
      auto const* priority = integer_member(item, "priority");
      auto const* phase    = string_member(item, "phase_slug");
      // A deferral must point at a phase this same cache proposes; pointing
      // anywhere else means the two halves disagree about the plan shape.
      if (priority == nullptr || *priority < 150 || phase == nullptr || !phase_slugs.contains(*phase))
        return std::nullopt;
    }
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
  // Report the caller's own spelling of `root`, not the symlink-resolved
  // `canonical` -- a shell keeps a symlink-spelled PWD, and the operator-
  // facing repo_root/repo_slug must key off that spelling, not realpath(3)
  // (task 6453, plan 351 task 2378). `canonical` remains the walk root
  // below: iterating the resolved directory is correct either way, and
  // guarantees this validated-existing directory is what gets scanned.
  auto display_root = root.is_absolute() ? root.lexically_normal() : canonical;
  if (display_root.filename().empty())
    display_root = display_root.parent_path();
  request req;
  req.repo_root    = display_root.string();
  req.repo_slug    = slugify(display_root.filename().string());
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
  // Mirrors `planar.engine.llm.client.cachePath` / `pendingPath` (and this
  // tree's own `bootstrap-synthesis` cache, wired identically in
  // `synthesize.cpp`): both files for one repo/kind pair live in the same
  // `<planar_home>/cache/<kind>/<repo_slug>/` directory. An earlier
  // `llm/import-interpretation/...` + `llm/pending/import-interpretation/
  // <slug>.json` layout diverged from the oracle on both the top-level
  // directory name and the pending-file location, which would have made the
  // real `pl-import` vendor-skill handoff write its result somewhere this
  // binary never looks.
  out.cache_path   = planar_home / "cache" / "import-interpretation" / req.repo_slug / (req.fingerprint + ".json");
  out.pending_path = planar_home / "cache" / "import-interpretation" / req.repo_slug / "_pending.json";
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
                            "`planar import --help` for the full contract.",
                            out.pending_path.string(), out.cache_path.string());
  return out;
}
} // namespace planar::engine::importer
