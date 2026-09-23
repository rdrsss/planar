/// @file brief.cpp
/// @brief `planar::engine::execute::brief` — the pure coder-brief compiler
/// behind `ctx.brief` (plan 996, task 6125).
///
/// Port target: `zig/src/cmd/planar-execute/brief.zig`'s `compileBrief`.
/// Section numbering in the comments below mirrors the oracle's own section
/// comments 1-through-8 so a reviewer can diff the two side by side.

module planar.engine_execute;

import std;

namespace planar::engine::execute::brief {

namespace {

/// @brief Append `"### {heading}\n\n"` plus one bullet block per evidence
/// row, or `"_(none)_\n\n"` when `values` is empty. Port target: `brief.zig`'s
/// `renderPacketEvidence`.
/// @param out The buffer being built.
/// @param heading The subsection heading.
/// @param values The evidence rows.
auto render_packet_evidence(std::string& out, std::string_view heading, std::vector<state::packet_evidence> const& values)
    -> void {
  out += std::format("### {}\n\n", heading);
  if (values.empty()) {
    out += "_(none)_\n\n";
    return;
  }
  for (auto const& value : values) {
    out += std::format("- `{}:{}` locator=`{}` status=`{}` freshness=`{}` required={} covered={}\n", value.kind, value.id,
                       value.locator, value.status, value.freshness, value.required, value.covered);
    out += std::format("  - provenance: `{}`\n", value.provenance);
    out += std::format("  - source digest: `{}`\n", value.source_digest);
    out += std::format("  - current digest: `{}`\n", value.current_digest);
    out += std::format("  - materializer: `{}` / current `{}`\n", value.materializer_version, value.current_materializer_version);
    if (!value.display_label.empty()) {
      out += std::format("  - display label: {}\n", value.display_label);
    }
    out += "  - exact text:\n";
    for (auto const line : std::views::split(value.text, '\n')) {
      out += std::format("    > {}\n", std::string_view{line.begin(), line.end()});
    }
  }
  out += '\n';
}

/// @brief Append each line of `text`, prefixed `"  > "`, for a verbatim
/// block-quote (spec-citation excerpts).
/// @param out The buffer being built.
/// @param text The excerpt.
auto render_blockquote(std::string& out, std::string_view text) -> void {
  for (auto const line : std::views::split(text, '\n')) {
    out += std::format("  > {}\n", std::string_view{line.begin(), line.end()});
  }
}

} // namespace

auto compile_brief(brief_inputs const& inputs) -> std::expected<std::string, brief_error> {
  std::optional<state::packet_evidence> authoritative_plan;
  std::optional<state::packet_evidence> authoritative_claim;

  if (inputs.authoritative_packet.has_value()) {
    auto const& authoritative = *inputs.authoritative_packet;
    bool const  identity_ok =
        authoritative.ready() && inputs.tasks.size() == 1 && inputs.tasks[0].id == authoritative.input.task_id &&
        inputs.tasks[0].status == authoritative.input.status && authoritative.input.owning_plans.size() == 1 &&
        authoritative.input.owning_plans[0].id == inputs.plan.id;
    if (!identity_ok) {
      return std::unexpected(brief_error::authoritative_identity_mismatch);
    }
    authoritative_plan = authoritative.input.owning_plans[0];
    for (auto const& claim : authoritative.input.claims) {
      if (claim.text == inputs.claim_token && claim.status == "active") {
        authoritative_claim = claim;
        break;
      }
    }
    if (!authoritative_claim.has_value()) {
      return std::unexpected(brief_error::authoritative_identity_mismatch);
    }
  }

  std::string out;
  out.reserve(4096);

  // ---------------------------------------------------------------------
  // Section 1 — Header: plan identity, task IDs, claim token.
  // ---------------------------------------------------------------------
  out += "# Coder Brief\n\n";

  if (authoritative_plan.has_value()) {
    auto const& label = authoritative_plan->display_label.empty() ? authoritative_plan->text : authoritative_plan->display_label;
    out += std::format("**Plan:** {} (id {})\n", label, authoritative_plan->id);
  } else {
    out += std::format("**Plan:** {} (id {})\n", inputs.plan.title, inputs.plan.id);
  }
  if (inputs.plan.slug.has_value()) {
    out += std::format("**Plan slug:** {}\n", *inputs.plan.slug);
  }
  out += std::format("**Plan status:** {}\n\n", authoritative_plan.has_value() ? authoritative_plan->status : inputs.plan.status);

  out += "**Task(s) dispatched:**\n";
  if (inputs.authoritative_packet.has_value()) {
    auto const& authoritative = *inputs.authoritative_packet;
    auto const& slug          = inputs.tasks[0].slug;
    if (slug.has_value()) {
      out += std::format("- task:{} — {} [slug: {}; status: {}]\n", authoritative.input.task_id, authoritative.input.title, *slug,
                         authoritative.input.status);
    } else {
      out += std::format("- task:{} — {} [status: {}]\n", authoritative.input.task_id, authoritative.input.title,
                         authoritative.input.status);
    }
  } else {
    for (auto const& task : inputs.tasks) {
      if (task.slug.has_value()) {
        out += std::format("- task:{} — {} [slug: {}]\n", task.id, task.title, *task.slug);
      } else {
        out += std::format("- task:{} — {}\n", task.id, task.title);
      }
    }
  }
  out += '\n';

  out +=
      std::format("**Claim token:** `{}`\n\n", authoritative_claim.has_value() ? authoritative_claim->text : inputs.claim_token);
  if (inputs.authoritative_packet.has_value()) {
    auto const& authoritative = *inputs.authoritative_packet;
    out += std::format("**Authoritative packet digest:** `{}`\n\n", authoritative.digest);
    out += std::format("**Authoritative task title:** {}\n\n", authoritative.input.title);
  }

  // ---------------------------------------------------------------------
  // Section 2 — Problem statement.
  // ---------------------------------------------------------------------
  out += "## Problem\n\n";
  out +=
      inputs.authoritative_packet.has_value() ? inputs.authoritative_packet->input.acceptance_criteria : inputs.problem_statement;
  out += "\n\n";

  // ---------------------------------------------------------------------
  // Section 3 — Spec citations ("Read firsthand").
  // ---------------------------------------------------------------------
  out += "## Read firsthand (do not paraphrase)\n\n";
  if (inputs.authoritative_packet.has_value()) {
    for (auto const& citation : inputs.authoritative_packet->input.citations) {
      out += std::format("- `{}`\n", citation.locator);
    }
    out += '\n';
  } else if (inputs.spec_citations.empty()) {
    out += "_(no spec citations for this cycle)_\n\n";
  } else {
    for (auto const& citation : inputs.spec_citations) {
      out += std::format("- `{}`\n", citation.path);
      if (citation.verbatim_slice.has_value()) {
        out += "  > (verbatim excerpt):\n";
        render_blockquote(out, *citation.verbatim_slice);
      }
    }
    out += '\n';
  }

  // ---------------------------------------------------------------------
  // Section 4 — Locked decisions (inline).
  // ---------------------------------------------------------------------
  out += "## Locked decisions\n\n";
  if (inputs.authoritative_packet.has_value()) {
    for (auto const& decision : inputs.authoritative_packet->input.decisions) {
      out += std::format("- **{}**: {}\n", decision.id, decision.text);
    }
    out += '\n';
  } else if (inputs.locked_decisions.empty()) {
    out += "_(no locked decisions for this cycle)_\n\n";
  } else {
    for (auto const& decision : inputs.locked_decisions) {
      out += std::format("- **{}**: {}\n", decision.id, decision.text);
    }
    out += '\n';
  }

  if (inputs.authoritative_packet.has_value()) {
    auto const& authoritative = *inputs.authoritative_packet;
    out += "## Authoritative packet context\n\n";
    out += std::format("**Task body:**\n\n{}\n\n", authoritative.input.body);
    out += std::format("**Exact next action:**\n\n{}\n\n", authoritative.input.next_action);
    render_packet_evidence(out, "Owning plans", authoritative.input.owning_plans);
    render_packet_evidence(out, "Anchor plans", authoritative.input.anchor_plans);
    render_packet_evidence(out, "Spec citations and source digests", authoritative.input.citations);
    render_packet_evidence(out, "Locked decisions", authoritative.input.decisions);
    render_packet_evidence(out, "Questions", authoritative.input.questions);
    render_packet_evidence(out, "Scenarios and coverage", authoritative.input.scenarios);
    render_packet_evidence(out, "Dependencies", authoritative.input.dependencies);
    render_packet_evidence(out, "Touched surfaces", authoritative.input.touches);
    render_packet_evidence(out, "Current claims", authoritative.input.claims);
    render_packet_evidence(out, "Materialized facts, provenance, and freshness", authoritative.input.facts);
  }

  // ---------------------------------------------------------------------
  // Section 4.5 — Prior-stage context.
  // ---------------------------------------------------------------------
  out += "## Prior-stage context\n\n";
  bool const has_capsule = inputs.context_capsule.has_value() && !inputs.context_capsule->empty();
  bool const has_records = !inputs.context_records.empty();
  if (!has_capsule && !has_records) {
    out += "_(no prior-stage context for this cycle)_\n\n";
  } else {
    if (has_capsule) {
      out += "### Compiled capsule\n\n";
      out += *inputs.context_capsule;
      out += "\n\n";
    }
    if (has_records) {
      out += "### Context records\n\n";
      for (auto const& record : inputs.context_records) {
        out += std::format("- **{}**: {}\n", record.kind, record.body);
      }
      out += '\n';
    }
  }

  // ---------------------------------------------------------------------
  // Section 5 — Available verbs/flags (from schema catalog).
  // ---------------------------------------------------------------------
  out += "## Available verbs (from schema catalog)\n\n";
  auto const& bin_root = inputs.agent_schema.root();
  bool        has_cmds = false;
  for (auto const& cmd : inputs.agent_schema.commands()) {
    if (cmd.command == bin_root || cmd.hidden) {
      continue;
    }
    has_cmds = true;
    out += std::format("- `{}`", cmd.command);
    if (!cmd.flags.empty()) {
      bool first = true;
      for (auto const& flag : cmd.flags) {
        if (first) {
          out += " — flags:";
          first = false;
        }
        out += std::format(" `{}`", flag.long_name);
        if (flag.required) {
          out += '*';
        }
        for (auto const& alias : flag.aliases) {
          out += std::format(" / `{}`", alias);
        }
      }
    }
    out += '\n';
  }
  if (!has_cmds) {
    out += "_(schema catalog is empty or contains only the root command)_\n";
  }
  out += "\n(`*` = required flag)\n\n";

  // ---------------------------------------------------------------------
  // Section 6 — Named gates.
  // ---------------------------------------------------------------------
  out += "## Gates (run all; paste counts verbatim in report)\n\n";
  if (inputs.authoritative_packet.has_value()) {
    for (auto const& gate : inputs.authoritative_packet->input.validation_gates) {
      out += std::format("- `{}`\n", gate.text);
    }
    out += '\n';
  } else if (inputs.gates.empty()) {
    out += "_(no gates specified — check the orchestrator brief)_\n\n";
  } else {
    for (auto const& gate : inputs.gates) {
      out += std::format("- `{}`\n", gate);
    }
    out += '\n';
  }

  // ---------------------------------------------------------------------
  // Section 7 — Work-complete report shape.
  // ---------------------------------------------------------------------
  out += "## Work-complete report (<=400 words)\n\n";
  out += "Return a report with ALL of the following sections (write \"N/A\" only\n";
  out += "if the section genuinely does not apply):\n\n";
  out += "1. **Files changed** — enumerated list with one-line description per file\n";
  out += "2. **Validation run** — commands executed and their outcomes (paste verbatim;\n";
  out += "   a claim of \"clean\" without command output is not valid)\n";
  out += "3. **Claim state** — claim token(s), last heartbeat, work stayed inside scope\n";
  out += "4. **Pre-flight checklist** — confirmation each methodology checklist item ran\n";
  out += "5. **Residual risk** — known gaps, assumptions, deferred items\n";
  out += "6. **Reviewer focus** — areas needing most scrutiny\n\n";

  // ---------------------------------------------------------------------
  // Section 8 — Terminal-verb instruction.
  // ---------------------------------------------------------------------
  out += "## Terminal verb\n\n";
  out += "End this cycle with **exactly one** terminal verb:\n\n";
  out += "- `planar-agent complete --claim <token>` — work succeeded\n";
  out += "- `planar-agent fail --claim <token> --reason <text>` — work failed\n";
  out += "- `planar-agent release --claim <token>` — graceful give-up\n";
  out += "- `planar-agent block --claim <token> --blocker <task-id> --reason <text>` — external blocker\n\n";
  out += "**block rather than ask when you hit an external blocker.**\n";
  out += "Do NOT call two terminal verbs.\n";

  return out;
}

} // namespace planar::engine::execute::brief
