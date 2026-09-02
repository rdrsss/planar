/// @file importer.cppm
/// @brief Filesystem discovery, deterministic transcription staging, and
/// pending/cache files for `planar import`.
module;

export module planar.engine.importer;

import std;

namespace planar::engine::importer {

/// @brief The result of scanning an import root without touching SQLite.
export struct request {
  std::string repo_root;       ///< Canonical repository root.
  std::string repo_slug;       ///< Directory-derived stable slug.
  std::string anchor_title;    ///< First README heading, or repo slug.
  std::string fingerprint;     ///< Stable request fingerprint.
  std::size_t docs_count = 0;  ///< Markdown files found outside guides.
  std::size_t guide_count = 0; ///< AGENTS/CLAUDE guide files found.
  std::size_t tree_count = 0;  ///< Regular filesystem entries observed.
};

/// @brief A staged/cached interpretation result visible to the handler.
export struct outcome {
  enum class mode { skipped, pending, cache_hit } mode_ = mode::skipped;
  request request_;
  std::filesystem::path cache_path;
  std::filesystem::path pending_path;
  std::string message;
};

/// @brief Why filesystem staging could not proceed.
export enum class error { not_found, invalid_input, io };

/// @brief Build the deterministic import request and, for interpretation,
/// discover or atomically stage its pending JSON request.
/// @param root Existing repository directory.
/// @param planar_home Operator state root for LLM handoff files.
/// @param interpret Whether the LLM cache/pending protocol is selected.
/// @return Read-only request outcome or a filesystem error.
export auto run(const std::filesystem::path& root, const std::filesystem::path& planar_home, bool interpret)
    -> std::expected<outcome, error>;

} // namespace planar::engine::importer
