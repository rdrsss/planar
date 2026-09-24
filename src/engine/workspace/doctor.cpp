/// @file doctor.cpp
/// @brief Implementation of `planar.engine.workspace.doctor` (plan 996, task
/// 6110). See doctor.cppm for why this verb is not read-only, where it actually
/// writes, and why `issues_found` is a length rather than a repair count.

module;

#include <glaze/glaze.hpp>

module planar.engine.workspace.doctor;

import std;
import planar.db;
import planar.json_text;
import planar.engine.workspace.identity;

namespace planar::engine::workspace::doctor {

namespace {

using planar::json_text::append_json_string;

constexpr std::string_view k_skip_suffix = "; skipping root guidance repair";

auto path_exists(const std::filesystem::path& path) -> bool {
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    return true;
  }
  const auto st = std::filesystem::symlink_status(path, ec);
  return !ec && st.type() != std::filesystem::file_type::not_found;
}

auto uncertain(std::string_view reason) -> shape_check {
  return {workspace_shape::uncertain, std::format("{}{}", reason, k_skip_suffix)};
}

auto finish(const identity::workspace& org, std::vector<issue> issues) -> org_report {
  // `issues_found` is literally the length. The two can never disagree, and the
  // array includes entries that were not repaired. See doctor.cppm.
  const auto count = static_cast<std::int64_t>(issues.size());
  return {org.slug, org.id, count, std::move(issues)};
}

} // namespace

auto classify_shape(db::connection& conn, std::int64_t org_id) -> std::optional<shape_check> {
  auto stmt = conn.prepare("select config_json from associations where id = ?");
  if (!stmt || !stmt->bind_int64(1, org_id)) {
    return std::nullopt;
  }
  const auto step = stmt->step();
  if (!step) {
    return std::nullopt;
  }
  if (*step == db::step_result::done) {
    return uncertain("workspace config row missing");
  }
  if (stmt->is_null(0)) {
    return uncertain("workspace config_json is missing");
  }
  const auto config_json = stmt->column_text(0);
  if (config_json.empty()) {
    return uncertain("workspace config_json is missing");
  }

  glz::generic parsed{};
  if (glz::read_json(parsed, config_json)) {
    return uncertain("workspace config_json is malformed");
  }
  if (!parsed.is_object()) {
    return uncertain("workspace config_json is not an object");
  }
  const auto& obj = parsed.get_object();

  // `root_path` is validated BEFORE `workspace_shape` is even read, which is
  // what makes `non_meta` always imply a usable root path.
  const auto root = obj.find("root_path");
  if (root == obj.end()) {
    return uncertain("workspace config_json has no root_path");
  }
  if (!root->second.is_string() || root->second.get_string().empty()) {
    return uncertain("workspace config_json has invalid root_path");
  }

  const auto shape = obj.find("workspace_shape");
  if (shape == obj.end()) {
    // An ABSENT shape is `sibling`, not uncertain: the polyrepo layout is the
    // default and predates the key.
    return shape_check{workspace_shape::non_meta, ""};
  }
  if (!shape->second.is_string()) {
    return uncertain("workspace config_json has invalid workspace_shape");
  }
  const auto value = shape->second.get_string();
  if (value == "meta-repo") {
    return shape_check{workspace_shape::meta_repo, ""};
  }
  if (value == "sibling") {
    return shape_check{workspace_shape::non_meta, ""};
  }
  return uncertain("workspace config_json has unknown workspace_shape");
}

auto diagnose(db::connection& conn, const identity::env_lookup& env, const identity::workspace& org) -> org_report {
  std::vector<issue> issues;

  const auto value = identity::load_layout(env, org.id);
  if (!value) {
    // EARLY RETURN: with no layout there is nothing further to check, and every
    // later diagnostic would be about paths that could not be computed.
    issues.push_back({"error", std::format("computing layout for org {}: HomeNotSet", org.id)});
    return finish(org, std::move(issues));
  }

  if (!path_exists(value->dir)) {
    if (!identity::ensure_layout(env, org.id)) {
      issues.push_back({"error", std::format("creating state dir {}: AccessDenied", value->dir.string())});
      return finish(org, std::move(issues));
    }
    issues.push_back({"fix", std::format("created state dir {}", value->dir.string())});
  }

  // `missing` entries are REPORTED, never repaired — doctor cannot generate
  // AGENTS.md, only `regenerate` can. That is why the detail names the verb.
  if (!path_exists(value->agents_md)) {
    issues.push_back({"missing", std::format("{} (run `planar workspace regenerate` after M3)", value->agents_md.string())});
  }
  if (!path_exists(value->routing_table)) {
    issues.push_back({"missing", std::format("{} (run `planar workspace regenerate` after M3)", value->routing_table.string())});
  }

  const auto shape = classify_shape(conn, org.id);
  if (!shape) {
    issues.push_back({"error", std::format("reading workspace config for org:{}: QueryFailed{}", org.slug, k_skip_suffix)});
    return finish(org, std::move(issues));
  }

  switch (shape->shape) {
  case workspace_shape::meta_repo:
    // A meta repo keeps its root-level instruction files under repo ownership.
    // The state dir above is still verified; the root is left alone entirely.
    break;
  case workspace_shape::uncertain:
    if (!shape->reason.empty()) {
      issues.push_back({"error", shape->reason});
    }
    break;
  case workspace_shape::non_meta: {
    if (!org.root_path) {
      // classify_shape() already validated root_path in config_json, so this is
      // only reachable if the two reads disagree. Reported rather than
      // dereferenced — the Zig original unwraps unconditionally here.
      issues.push_back({"error", std::format("workspace config_json has invalid root_path{}", k_skip_suffix)});
      break;
    }
    const std::filesystem::path root{*org.root_path};
    const auto                  dirty = identity::dirty_links(root, value->agents_md);
    if (dirty.empty()) {
      break;
    }
    const auto strategy = identity::install_symlinks(root, *value);
    if (!strategy) {
      issues.push_back({"error", std::format("installing symlinks at {}: AccessDenied", org.root_path.value())});
      return finish(org, std::move(issues));
    }
    for (const auto& name : dirty) {
      // The arrow is U+2192, three UTF-8 bytes, and it is in the oracle's
      // capture rather than decoration this port added.
      issues.push_back(
          {"fix", std::format("reinstalled {} {}/{} → {}", *strategy, org.root_path.value(), name, value->agents_md.string())});
    }
    break;
  }
  }

  return finish(org, std::move(issues));
}

auto run(db::connection& conn, const identity::env_lookup& env) -> std::optional<std::vector<org_report>> {
  const auto orgs = identity::list_orgs(conn);
  if (!orgs) {
    return std::nullopt;
  }
  std::vector<org_report> out;
  for (const auto& org : *orgs) {
    out.push_back(diagnose(conn, env, org));
  }
  return out;
}

auto doctor_json(std::span<const org_report> reports) -> std::string {
  std::string out = "{\"orgs\":[";
  for (std::size_t i = 0; i < reports.size(); ++i) {
    const auto& report = reports[i];
    if (i != 0) {
      out.push_back(',');
    }
    out.append("{\"slug\":");
    append_json_string(out, report.slug);
    out.append(std::format(",\"org_id\":{},\"issues_found\":{},\"issues_repaired\":[", report.org_id, report.issues_found));
    for (std::size_t j = 0; j < report.issues.size(); ++j) {
      if (j != 0) {
        out.push_back(',');
      }
      out.append("{\"kind\":");
      append_json_string(out, report.issues[j].kind);
      out.append(",\"detail\":");
      append_json_string(out, report.issues[j].detail);
      out.push_back('}');
    }
    out.append("]}");
  }
  out.append("]}\n");
  return out;
}

auto doctor_text(std::span<const org_report> reports) -> std::string {
  // NOTE there is no header and no empty-state sentence: an empty database
  // produces ZERO BYTES here while `--json` produces `{"orgs":[]}`.
  std::string out;
  for (const auto& report : reports) {
    for (const auto& item : report.issues) {
      out.append(std::format("{}: {}\n", item.kind, item.detail));
    }
    if (report.issues_found == 0) {
      out.append(std::format("org:{} ok\n", report.slug));
    } else {
      out.append(std::format("org:{} repaired {} issues\n", report.slug, report.issues_found));
    }
  }
  return out;
}

auto no_orgs_error() -> std::string {
  return "error: no org associations registered; create one with `planar workspace init`\n";
}

auto ambiguous_orgs_error() -> std::string {
  return "error: multiple org associations registered; pass the workspace slug or id explicitly\n";
}

} // namespace planar::engine::workspace::doctor
