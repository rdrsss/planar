/// @file materialize.cppm
/// @brief `planar.engine.ingest.materialize` — the digest and Markdown-section
/// helpers shared by the packet's references and `task packet --expand`.
///
/// This module no longer writes anything. It once staged a fact row for every
/// task below an anchor plan, in a table migration 00043 dropped; the packet
/// cites through the task's `cites` entity links. What remains is
/// pure: a SHA-256 wrapper, the reference digest, and the heading-based
/// section resolver `--expand` uses. The functions touch no database and
/// return `std::optional` for absence; none throws.

module;

export module planar.engine.ingest.materialize;

import std;

namespace planar::engine::ingest::materialize {

/// @brief Computes a reference's digest.
///
/// SHA-256 over `"<kind>\0<id>\0<locator>\0<semantic source>"`, lowercase hex.
/// The packet digests a cited artifact as `("artifact", id, "body", body)`.
/// @param source_kind The source entity's kind.
/// @param source_id The source entity's id.
/// @param locator The locator within that entity.
/// @param semantic_source The semantic content the digest covers.
/// @return The 64-character lowercase hex digest.
export [[nodiscard]] auto source_digest(std::string_view source_kind, std::int64_t source_id, std::string_view locator,
                                        std::string_view semantic_source) -> std::string;

/// @brief `sha256(input)` as 64 lowercase hex characters (FIPS 180-4).
///
/// A thin wrapper over layer-1 `planar.sha256`, kept so `packet` digests
/// whole texts through the module it already imports.
/// @param input The bytes to digest.
/// @return The 64-character lowercase hex digest.
export [[nodiscard]] auto sha256_hex(std::string_view input) -> std::string;

/// @brief Resolves an `<anything>#<section name>` locator against a document.
///
/// Heading matching is case-insensitive and honors nesting: the section runs
/// until the next heading at the SAME OR SHALLOWER level, so a `### Nested`
/// under a cited `## Target` stays part of the section. Headings inside fenced
/// or indented code blocks are ignored entirely, so a decoy heading in an
/// example block neither opens nor closes a section.
/// @param body The document to search.
/// @param locator The locator whose `#` fragment names the section.
/// @return A view INTO `body` (which must outlive the result), or `nullopt`
/// when the document has no such section.
export [[nodiscard]] auto artifact_section(std::string_view body, std::string_view locator) -> std::optional<std::string_view>;

} // namespace planar::engine::ingest::materialize
