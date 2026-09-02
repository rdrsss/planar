/// @file importer.cpp
/// @brief Implementation of the filesystem-only import staging engine.
module planar.engine.importer;

import std;
import planar.json_text;

namespace planar::engine::importer {
namespace {
auto slugify(std::string_view raw) -> std::string {
  std::string out;
  bool dash = false;
  for (unsigned char c : raw) {
    if (std::isalnum(c)) { out.push_back(static_cast<char>(std::tolower(c))); dash = false; }
    else if (!out.empty() && !dash) { out.push_back('-'); dash = true; }
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  return out.empty() ? "repo" : out;
}
auto first_heading(const std::filesystem::path& root, std::string_view fallback) -> std::string {
  std::ifstream in(root / "README.md", std::ios::binary);
  std::string line;
  while (std::getline(in, line)) {
    auto const start = line.find_first_not_of(" \t\r");
    if (start != std::string::npos && line[start] == '#') {
      auto pos = line.find_first_not_of("# \t", start);
      if (pos != std::string::npos) return line.substr(pos);
    }
  }
  return std::string{fallback};
}
auto fnv_hex(std::string_view input) -> std::string {
  std::uint64_t value = 14695981039346656037ULL;
  for (unsigned char c : input) { value ^= c; value *= 1099511628211ULL; }
  return std::format("{:016x}", value);
}
auto write_atomic(const std::filesystem::path& path, std::string_view body) -> bool {
  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);
  if (ec) return false;
  auto const temp = path.string() + ".tmp";
  { std::ofstream out(temp, std::ios::binary | std::ios::trunc); if (!out) return false; out << body; if (!out) return false; }
  std::filesystem::rename(temp, path, ec);
  if (ec) { std::filesystem::remove(path, ec); ec.clear(); std::filesystem::rename(temp, path, ec); }
  return !ec;
}
} // namespace

auto run(const std::filesystem::path& root, const std::filesystem::path& planar_home, bool interpret)
    -> std::expected<outcome, error> {
  std::error_code ec;
  if (root.empty()) return std::unexpected(error::invalid_input);
  auto const canonical = std::filesystem::canonical(root, ec);
  if (ec || !std::filesystem::is_directory(canonical, ec)) return std::unexpected(error::not_found);
  request req;
  req.repo_root = canonical.string();
  req.repo_slug = slugify(canonical.filename().string());
  req.anchor_title = first_heading(canonical, req.repo_slug);
  std::string fingerprint_input = req.repo_slug + '\0' + req.anchor_title;
  for (std::filesystem::recursive_directory_iterator it(canonical, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_directory(ec) || ec) continue;
    ++req.tree_count;
    auto const name = it->path().filename().string();
    auto const ext = it->path().extension().string();
    if (name == "AGENTS.md" || name == "CLAUDE.md") ++req.guide_count;
    else if (ext == ".md" || ext == ".mdx") ++req.docs_count;
    fingerprint_input += '\0' + std::filesystem::relative(it->path(), canonical, ec).string();
  }
  if (ec) return std::unexpected(error::io);
  req.fingerprint = fnv_hex(fingerprint_input);
  outcome out{.request_ = req};
  if (!interpret) {
    out.message = "import: interpretation disabled; run with --interpret to stage/consume LLM artifacts.";
    return out;
  }
  out.cache_path = planar_home / "llm" / "import-interpretation" / req.repo_slug / (req.fingerprint + ".json");
  out.pending_path = planar_home / "llm" / "pending" / "import-interpretation" / (req.repo_slug + ".json");
  if (std::filesystem::exists(out.cache_path, ec) && !ec) {
    out.mode_ = outcome::mode::cache_hit;
    out.message = std::format("Loaded cached interpretation result: {}", out.cache_path.string());
    return out;
  }
  std::string body = "{\"schema_version\":1,\"repo_slug\":" + json_text::json_string(req.repo_slug) +
                     ",\"repo_root\":" + json_text::json_string(req.repo_root) + ",\"fingerprint\":" +
                     json_text::json_string(req.fingerprint) + "}\n";
  if (!write_atomic(out.pending_path, body)) return std::unexpected(error::io);
  out.mode_ = outcome::mode::pending;
  out.message = std::format("Awaiting LLM interpretation. The vendor skill should:\n  1. read  {}\n  2. run the LLM at temperature 0\n  3. write the Result to {}\n  4. re-invoke `planar import <repo> --interpret`\nSee `commands/claude/pl-import.md` for the full contract.", out.pending_path.string(), out.cache_path.string());
  return out;
}
} // namespace planar::engine::importer
