// @file json_envelope_test_support.hpp
// @brief Shared assertion helper for the additive `--json` error envelope
// (task 6844, decision 1145, supersedes D5): builds the exact one-line
// stdout payload a failing handler writes under `--json`, so leaf tests can
// assert it precisely -- `out == json_error_envelope_line(verb, tag)` --
// instead of the weaker `out.empty() == false` decision 1145 obsoleted.
//
// A plain header, not a module, and not tied to any single result type:
// each leaf `.t.cpp` in this directory defines its OWN local
// `invocation`/`capture` struct (deliberately duplicated per file rather
// than shared -- see e.g. `assoc_detect.t.cpp`'s own `invocation`), so this
// takes the raw stdout string rather than a shared result type. Nothing
// that ships includes this header; only `.t.cpp` translation units do.
#pragma once

namespace planar::cmd::testsupport {

/// @brief The exact stdout line `report_json_envelope` writes for `verb`
/// and `tag` (see `src/cmd/planar/exit.cppm` and
/// `src/cmd/planar-agent/exit.cppm`). Every `verb`/`tag` pair actually
/// passed by a call site in this tree is a plain identifier or a bare
/// space-separated verb path -- no quote, backslash, or control character
/// -- so no JSON escaping is performed here. A future call site with a
/// verb/tag that needs escaping must escape it before calling.
/// @param verb The resolved verb path, e.g. `"task packet"`.
/// @param tag The envelope's tag, e.g. `"not_found"`.
/// @return The exact one-line stdout payload, newline included.
inline auto json_error_envelope_line(std::string_view verb, std::string_view tag) -> std::string {
  return std::string(R"({"error":{"verb":")") + std::string(verb) + R"(","tag":")" + std::string(tag) + R"("}})" + "\n";
}

} // namespace planar::cmd::testsupport
