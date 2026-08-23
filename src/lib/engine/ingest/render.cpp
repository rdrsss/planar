/// @file render.cpp
/// @brief Implementation of `planar.engine.ingest.render` (see render.cppm).
///
/// Every literal below — spacing, punctuation, separator, newline — is part of
/// the contract this module ports. The JSON path in particular is the M9
/// parity gate; see the byte-exactness tests in render.t.cpp.

module planar.engine.ingest.render;

import std;
import planar.json_text;
import planar.engine.ingest.diff;
import planar.engine.ingest.coverage;

namespace planar::engine::ingest::render {

// The one shared escape table, layer 1. See json_text.cppm.
using json_text::append_json_string;
namespace {

constexpr std::size_t display_width = 36;

/// @brief The single-character prefix each op renders as in the text preview.
[[nodiscard]] auto op_prefix(diff::op value) -> std::string_view {
  switch (value) {
  case diff::op::add:
    return "+";
  case diff::op::update:
    return "~";
  case diff::op::remove:
    return "-";
  }
  return "+";
}

/// @brief Produces a display-safe string at most `max_len` bytes long, ending
/// in `...` when the original was longer.
[[nodiscard]] auto truncate(std::string_view s, std::size_t max_len) -> std::string {
  if (s.size() <= max_len) {
    return std::string{s};
  }
  std::string out{s.substr(0, max_len - 3)};
  out.append("...");
  return out;
}

/// @brief Left-aligns `s` in a field of `width`, never truncating.
///
/// Mirrors Zig's `{s: <N}` — a value already at or past the width is emitted
/// unpadded rather than clipped (callers truncate first when they want that).
[[nodiscard]] auto pad_right(std::string_view s, std::size_t width) -> std::string {
  std::string out{s};
  if (out.size() < width) {
    out.append(width - out.size(), ' ');
  }
  return out;
}

/// @brief Appends one entity object to the JSON `entities` array.
///
/// `scope`, `derives_from` and `touches` are omitted entirely when empty,
/// matching the original's conditional emission — an empty-string field and an
/// absent field are different bytes.
auto append_entity(std::string& out, bool& first, std::string_view op, std::string_view kind, std::string_view title,
                   std::string_view scope, std::string_view derives_from, std::span<const std::string> touches) -> void {
  if (!first) {
    out.append(",\n");
  }
  first = false;
  out.append(std::format(R"(    {{"op": "{}", "kind": "{}", "title": )", op, kind));
  append_json_string(out, title);
  if (!scope.empty()) {
    out.append(", \"scope\": ");
    append_json_string(out, scope);
  }
  if (!derives_from.empty()) {
    out.append(", \"derives_from\": ");
    append_json_string(out, derives_from);
  }
  if (!touches.empty()) {
    out.append(", \"touches\": [");
    for (std::size_t i = 0; i < touches.size(); ++i) {
      if (i > 0) {
        out.append(", ");
      }
      append_json_string(out, touches[i]);
    }
    out.push_back(']');
  }
  out.append("}");
}

/// @brief Appends the `coverage:` summary line to the text preview.
auto append_coverage_line(std::string& out, const coverage::coverage& cov) -> void {
  out.append(std::format("coverage: {} tasks ({} with slug, {} without)", cov.total_tasks_, cov.tasks_with_slug_,
                         cov.tasks_without_slug_));
  if (!cov.uncovered_task_slugs_.empty()) {
    out.append(std::format("; {} uncovered: ", cov.uncovered_task_slugs_.size()));
    for (std::size_t i = 0; i < cov.uncovered_task_slugs_.size(); ++i) {
      if (i > 0) {
        out.append(", ");
      }
      out.append(cov.uncovered_task_slugs_[i]);
    }
  }
  if (!cov.orphan_scenarios_.empty()) {
    out.append(std::format("; {} orphan scenarios: ", cov.orphan_scenarios_.size()));
    for (std::size_t i = 0; i < cov.orphan_scenarios_.size(); ++i) {
      if (i > 0) {
        out.append("; ");
      }
      out.append(cov.orphan_scenarios_[i]);
    }
  }
  out.push_back('\n');
}

} // namespace

auto render_text(const diff::diff& d, bool applied) -> std::string {
  std::string out;

  if (!d.assoc_slug_.empty()) {
    out.append(std::format("{}/{}/\n", d.assoc_slug_, d.anchor_slug_));
  } else {
    out.append(std::format("plan:{}/{}/\n", d.anchor_plan_id_, d.anchor_slug_));
  }

  for (const auto& plan : d.child_plans_) {
    out.append(std::format("  {} plan       {} ({} tasks)\n", op_prefix(plan.op_),
                           pad_right(truncate(plan.title_, display_width), display_width), plan.tasks_.size()));
    for (const auto& task : plan.tasks_) {
      out.append(
          std::format("  {}   task     {}", op_prefix(task.op_), pad_right(truncate(task.title_, display_width), display_width)));
      if (!task.touches_.empty()) {
        out.append("  touches=");
        for (std::size_t i = 0; i < task.touches_.size(); ++i) {
          if (i > 0) {
            out.push_back(',');
          }
          out.append(task.touches_[i]);
        }
      }
      out.push_back('\n');
    }
  }

  for (const auto& plan : d.orphan_plans_) {
    out.append(std::format("  - plan       {} not in current roadmap        [needs --apply-removals]\n",
                           pad_right(truncate(plan.title_, display_width), display_width)));
    for (const auto& task : plan.tasks_) {
      out.append(std::format("  -   task     {} not in current roadmap        [needs --apply-removals]\n",
                             pad_right(truncate(task.title_, display_width), display_width)));
    }
  }
  for (const auto& task : d.orphan_tasks_) {
    out.append(std::format("  - task       {} not in current roadmap        [needs --apply-removals]\n",
                           pad_right(truncate(task.title_, display_width), display_width)));
  }

  for (const auto& decision : d.decisions_) {
    const std::string_view note = decision.op_ == diff::op::remove ? "  [needs --apply-removals]" : "";
    out.append(std::format("  {} {} {}{}\n", op_prefix(decision.op_), pad_right("decision", 10),
                           pad_right(truncate(decision.title_, display_width), display_width), note));
  }

  for (const auto& q : d.new_questions_) {
    const std::string_view note = !q.resolution_.empty() ? "  (will start answered)" : "";
    out.append(std::format("  + {} {}{}\n", pad_right("question", 10),
                           pad_right(truncate(q.title_, display_width), display_width), note));
  }
  for (const auto& change : d.updated_question_status_) {
    out.append(std::format("  ~ {} {}  [{} → {}]\n", pad_right("question", 10),
                           pad_right(truncate(change.question_title_, display_width), display_width), change.old_status_,
                           change.new_status_));
  }

  out.push_back('\n');
  const auto adds     = d.total_additions();
  const auto updates  = d.total_updates();
  const auto removals = d.total_removals();
  out.append(std::format("{} additions, {} updates, {} proposed removals.\n", adds, updates, removals));

  append_coverage_line(out, coverage::compute(d));

  if (!d.slug_collisions_.empty()) {
    out.append(std::format("slug-collisions: {} task slug(s) already exist globally:\n", d.slug_collisions_.size()));
    for (const auto& collision : d.slug_collisions_) {
      out.append(std::format("  conflict: slug '{}' already held by task {} (plan {}) — apply will fail with SlugConflict\n",
                             collision.slug_, collision.existing_task_id_, collision.existing_plan_id_));
    }
  }

  if (applied) {
    // Suppressed — the caller prints the applied-statistics line instead.
  } else if (adds > 0 || updates > 0 || removals > 0) {
    out.append("Run with --apply to commit; add --apply-removals to cancel proposed removals.\n");
  } else {
    out.append("Nothing to do.\n");
  }
  return out;
}

auto render_json(const diff::diff& d) -> std::string {
  const auto cov = coverage::compute(d);

  const auto scope_label =
      !d.assoc_slug_.empty() ? std::format("assoc:{}", d.assoc_slug_) : std::format("plan:{}", d.anchor_plan_id_);
  const auto anchor_derives = std::format("plan:{}", d.anchor_plan_id_);

  std::string out;
  out.append(std::format("{{\n  \"anchor_plan_id\": {},\n  \"assoc_slug\": ", d.anchor_plan_id_));
  append_json_string(out, d.assoc_slug_);
  out.append(",\n  \"anchor_slug\": ");
  append_json_string(out, d.anchor_slug_);

  out.append(",\n  \"entities\": [\n");
  bool first = true;
  for (const auto& plan : d.child_plans_) {
    append_entity(out, first, diff::to_string(plan.op_), "plan", plan.title_, scope_label, anchor_derives, {});
    for (const auto& task : plan.tasks_) {
      const auto derives = std::format("plan:{}", plan.title_);
      append_entity(out, first, diff::to_string(task.op_), "task", task.title_, scope_label, derives, task.touches_);
    }
  }
  for (const auto& plan : d.orphan_plans_) {
    append_entity(out, first, diff::to_string(plan.op_), "plan", plan.title_, scope_label, "", {});
    for (const auto& task : plan.tasks_) {
      append_entity(out, first, diff::to_string(task.op_), "task", task.title_, scope_label, "", {});
    }
  }
  for (const auto& task : d.orphan_tasks_) {
    append_entity(out, first, diff::to_string(task.op_), "task", task.title_, scope_label, "", {});
  }
  for (const auto& decision : d.decisions_) {
    append_entity(out, first, diff::to_string(decision.op_), "decision", decision.title_, scope_label, anchor_derives, {});
  }
  for (const auto& q : d.new_questions_) {
    append_entity(out, first, "add", "question", q.title_, scope_label, "", {});
  }
  for (const auto& change : d.updated_question_status_) {
    append_entity(out, first, "update", "question", change.question_title_, scope_label, "", {});
  }
  out.append("\n  ],\n");

  out.append(std::format("  \"summary\": {{\n    \"additions\": {},\n    \"updates\": {},\n    \"removals\": {}\n  }},\n",
                         d.total_additions(), d.total_updates(), d.total_removals()));

  out.append(std::format("  \"coverage\": {{\n    \"total_tasks\": {},\n    \"tasks_with_slug\": {},\n"
                         "    \"tasks_without_slug\": {},\n    \"uncovered_task_slugs\": [",
                         cov.total_tasks_, cov.tasks_with_slug_, cov.tasks_without_slug_));
  for (std::size_t i = 0; i < cov.uncovered_task_slugs_.size(); ++i) {
    if (i > 0) {
      out.append(", ");
    }
    append_json_string(out, cov.uncovered_task_slugs_[i]);
  }
  out.append("],\n    \"orphan_scenarios\": [");
  for (std::size_t i = 0; i < cov.orphan_scenarios_.size(); ++i) {
    if (i > 0) {
      out.append(", ");
    }
    append_json_string(out, cov.orphan_scenarios_[i]);
  }
  // Plain append, so the brace is literal — this is NOT a format string, and
  // writing `}}` here (Zig's escape, which the original does need) would emit
  // a stray second brace and break every JSON consumer.
  out.append("]\n  },\n");

  out.append("  \"slug_collisions\": [");
  for (std::size_t i = 0; i < d.slug_collisions_.size(); ++i) {
    if (i > 0) {
      out.append(", ");
    }
    out.append("{\"slug\": ");
    append_json_string(out, d.slug_collisions_[i].slug_);
    out.append(std::format(", \"existing_task_id\": {}, \"existing_plan_id\": {}}}", d.slug_collisions_[i].existing_task_id_,
                           d.slug_collisions_[i].existing_plan_id_));
  }
  out.append("]\n}\n");
  return out;
}

} // namespace planar::engine::ingest::render
