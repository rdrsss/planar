/// @file render.cpp
/// @brief Implementation of `planar.engine.templates.render` (plan 996,
/// task 6190). See render.cppm for the supported directive surface, the
/// error-name wire format, and the reproduced 128-byte truthiness defect.

module planar.engine.templates.render;

import std;
import planar.engine.templates.context;
import planar.json_dom;

namespace planar::engine::templates {

using json_dom::json_kind;
using json_dom::json_value;

namespace {

/// @brief What `{{.}}` and the shadowing `{{.Title}}`/`{{.ExternalKey}}`
/// bind to inside a `range` body. Absent outside one.
///
/// Go's `text/template` binds `.` to the top-level context outside a
/// `range`; this port does NOT, matching the Zig oracle, where a bare
/// `{{.}}` at top level is `unknown_field`. No shipped template uses that
/// form.
struct current_item {
  bool        is_child = false; ///< `true` for a `{{range .Children}}` element, `false` for a `{{range .Touches}}` one.
  std::string text;             ///< The repo slug, or the child's title.
  std::string external_key;     ///< The child's external key; unused when `is_child` is false.
};

using item_ref = const current_item*;

/// @brief Trim ASCII spaces and tabs from both ends.
///
/// zig's `std.mem.trim(u8, x, " \t")` — spaces and TABS only. A directive
/// padded with a newline is NOT trimmed and does not match `end`.
/// @param text The text to trim.
/// @return The trimmed view.
auto trim_directive(std::string_view text) -> std::string_view {
  auto const first = text.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return {};
  }
  auto const last = text.find_last_not_of(" \t");
  return text.substr(first, last - first + 1);
}

auto resolve_reference(std::string_view directive, const render_context& ctx, item_ref item)
    -> std::expected<std::string, render_error>;

auto exec_range(std::string_view src, std::size_t& cursor, const render_context& ctx, item_ref item, std::string& out)
    -> std::expected<void, render_error>;

/// @brief Render one `plan_info` field. `rest` is the path AFTER the head,
/// e.g. `Title` for `{{.Feature.Title}}`.
/// @param p The plan.
/// @param rest The remaining path.
/// @return The rendered text.
auto render_plan_field(const plan_info& p, std::string_view rest) -> std::expected<std::string, render_error> {
  // A bare `{{.Feature}}` is UNSUPPORTED rather than unknown — the oracle
  // distinguishes "you named a struct where a scalar goes" from "you named
  // nothing at all", and `templates validate` prints the difference.
  if (rest.empty()) {
    return std::unexpected(render_error::unsupported_directive);
  }
  if (rest == "ID") {
    return std::to_string(p.id);
  }
  if (rest == "Slug") {
    return p.slug;
  }
  if (rest == "Title") {
    return p.title;
  }
  if (rest == "Body") {
    return p.body;
  }
  if (rest == "Status") {
    return p.status;
  }
  if (rest == "ScopeKind") {
    return p.scope_kind;
  }
  if (rest == "ScopeID") {
    return std::to_string(p.scope_id);
  }
  return std::unexpected(render_error::unknown_field);
}

/// @brief Render one `task_info` field.
/// @param t The task.
/// @param rest The remaining path.
/// @return The rendered text.
auto render_task_field(const task_info& t, std::string_view rest) -> std::expected<std::string, render_error> {
  if (rest.empty()) {
    return std::unexpected(render_error::unsupported_directive);
  }
  if (rest == "ID") {
    return std::to_string(t.id);
  }
  if (rest == "Title") {
    return t.title;
  }
  if (rest == "Body") {
    return t.body;
  }
  if (rest == "Status") {
    return t.status;
  }
  if (rest == "Priority") {
    return std::to_string(t.priority);
  }
  if (rest == "ScopeKind") {
    return t.scope_kind;
  }
  if (rest == "ScopeID") {
    return std::to_string(t.scope_id);
  }
  return std::unexpected(render_error::unknown_field);
}

/// @brief Render one `scenario_info` field.
/// @param s The scenario.
/// @param rest The remaining path.
/// @return The rendered text.
auto render_scenario_field(const scenario_info& s, std::string_view rest) -> std::expected<std::string, render_error> {
  if (rest.empty()) {
    return std::unexpected(render_error::unsupported_directive);
  }
  if (rest == "ID") {
    return std::to_string(s.id);
  }
  if (rest == "Title") {
    return s.title;
  }
  if (rest == "Body") {
    return s.body;
  }
  return std::unexpected(render_error::unknown_field);
}

/// @brief Render one `assoc_info` field.
/// @param a The association.
/// @param rest The remaining path.
/// @return The rendered text.
auto render_assoc_field(const assoc_info& a, std::string_view rest) -> std::expected<std::string, render_error> {
  if (rest.empty()) {
    return std::unexpected(render_error::unsupported_directive);
  }
  if (rest == "Slug") {
    return a.slug;
  }
  if (rest == "Name") {
    return a.name;
  }
  return std::unexpected(render_error::unknown_field);
}

auto resolve_reference(std::string_view directive, const render_context& ctx, item_ref item)
    -> std::expected<std::string, render_error> {
  if (directive == ".") {
    if (item == nullptr) {
      return std::unexpected(render_error::unknown_field);
    }
    // For a child element `{{.}}` is its TITLE, not its key or a struct
    // dump. That is the oracle's `renderCurrentItem`.
    return item->text;
  }
  if (directive.size() < 2 || directive.front() != '.') {
    return std::unexpected(render_error::unknown_field);
  }
  auto const path = directive.substr(1);
  auto const dot  = path.find('.');
  auto const head = dot == std::string_view::npos ? path : path.substr(0, dot);
  auto const rest = dot == std::string_view::npos ? std::string_view{} : path.substr(dot + 1);

  if (head == "Feature") {
    return render_plan_field(ctx.feature, rest);
  }
  if (head == "Plan") {
    return render_plan_field(ctx.plan, rest);
  }
  if (head == "Task") {
    return render_task_field(ctx.task, rest);
  }
  if (head == "Scenario") {
    return render_scenario_field(ctx.scenario, rest);
  }
  if (head == "Assoc") {
    return render_assoc_field(ctx.assoc, rest);
  }
  if (head == "ExternalKey") {
    if (!rest.empty()) {
      return std::unexpected(render_error::unknown_field);
    }
    // Inside `{{range .Children}}` the CHILD's key shadows the top-level
    // one. A `{{range .Touches}}` element has no key of its own and falls
    // through to the context's.
    if (item != nullptr && item->is_child) {
      return item->external_key;
    }
    return ctx.external_key;
  }
  if (head == "Touches") {
    if (!rest.empty()) {
      return std::unexpected(render_error::unknown_field);
    }
    // `text/template` would print a slice as `[a b c]`. The oracle refuses
    // instead, so a bare `{{.Touches}}` is a validation issue rather than a
    // surprising payload. Oracle-captured as `UnsupportedDirective`.
    return std::unexpected(render_error::unsupported_directive);
  }
  if (head == "Title") {
    // Only reachable as the child-shadowed form inside
    // `{{range .Children}}{{.Title}}{{end}}`. A top-level `{{.Title}}` is
    // unknown, and so is `{{.Title}}` inside a `.Touches` range.
    if (item != nullptr && item->is_child && rest.empty()) {
      return item->text;
    }
    return std::unexpected(render_error::unknown_field);
  }
  return std::unexpected(render_error::unknown_field);
}

/// @brief Decide whether `{{if <path>}}`'s body is emitted.
///
/// A value is truthy when its RENDERED TEXT is non-empty — so `0` and
/// `false` are truthy (they render as `"0"` / `"false"`), which is NOT
/// Go's rule but is the oracle's. An unknown field is falsy rather than an
/// error; every other resolution error propagates.
///
/// FIXED AT TASK 6210 (decision 1067's FIX set). This used to fail the
/// ENTIRE render with `out_of_memory` when the referenced value exceeded 128
/// bytes: zig's `evalTruthy` resolved into a `var tmp: [128]u8` through a
/// FixedBufferAllocator, and the allocation failure escaped all the way out.
/// Bisected to exactly 128 pass / 129 fail -- a fixed budget, not a heap
/// condition.
///
/// The blast radius is why it was in the FIX set: truthiness does not depend
/// on length, so every task whose BODY exceeded 128 bytes was unrenderable
/// through any template guarding it with `{{if}}` -- including the shipped
/// `templates/defaults/github-issues/issue.json`. An operator saw a whole
/// propagation fail with `OutOfMemory` and nothing naming the field.
/// @param ctx The data.
/// @param item The current `range` binding, if any.
/// @param path The referenced path.
/// @return Whether to emit the body.
auto eval_truthy(const render_context& ctx, item_ref item, std::string_view path) -> std::expected<bool, render_error> {
  auto const resolved = resolve_reference(path, ctx, item);
  if (!resolved.has_value()) {
    if (resolved.error() == render_error::unknown_field) {
      return false;
    }
    return std::unexpected(resolved.error());
  }
  // No length ceiling. Emptiness is the whole question; the oracle's
  // 128-byte scratch buffer was an implementation artifact of how it
  // resolved the value, never part of the rule.
  return !resolved->empty();
}

/// @brief Scan forward from `probe` for the `{{end}}` matching an already
/// open `range`/`if`, tracking nesting.
/// @param src The source text.
/// @param probe Where to start scanning (just past the opening directive).
/// @param body_start Where the body began.
/// @param body Set to the body text on success.
/// @param after Set to the offset just past the `{{end}}` on success.
/// @return Nothing, or `unclosed_directive`.
auto find_matching_end(std::string_view src, std::size_t probe, std::size_t body_start, std::string_view& body,
                       std::size_t& after) -> std::expected<void, render_error> {
  std::size_t depth = 1;
  while (true) {
    auto const open = src.find("{{", probe);
    if (open == std::string_view::npos) {
      return std::unexpected(render_error::unclosed_directive);
    }
    auto const close = src.find("}}", open + 2);
    if (close == std::string_view::npos) {
      return std::unexpected(render_error::unclosed_directive);
    }
    auto const inner = trim_directive(src.substr(open + 2, close - (open + 2)));
    if (inner.starts_with("range ") || inner.starts_with("if ")) {
      ++depth;
    } else if (inner == "end") {
      --depth;
      if (depth == 0) {
        body  = src.substr(body_start, open - body_start);
        after = close + 2;
        return {};
      }
    }
    probe = close + 2;
  }
}

/// @brief Render `body` once with `item` bound.
/// @param body The loop body.
/// @param ctx The data.
/// @param item The binding.
/// @param out The output buffer.
/// @return Nothing, or the first error.
auto render_body(std::string_view body, const render_context& ctx, item_ref item, std::string& out)
    -> std::expected<void, render_error> {
  std::size_t cursor = 0;
  return exec_range(body, cursor, ctx, item, out);
}

/// @brief Execute `{{range <path>}}`.
///
/// Only the two named slices iterate. `{{range .Anything}}` else is
/// `unknown_field` — there is no generic slice walk, because the context
/// has no other slice to walk.
/// @param src The source text.
/// @param cursor Advanced past the matching `{{end}}`.
/// @param ctx The data.
/// @param out The output buffer.
/// @param path The referenced slice.
/// @return Nothing, or the first error.
auto exec_loop(std::string_view src, std::size_t& cursor, const render_context& ctx, std::string& out, std::string_view path)
    -> std::expected<void, render_error> {
  std::string_view body;
  std::size_t      after = 0;
  if (auto found = find_matching_end(src, cursor, cursor, body, after); !found.has_value()) {
    return found;
  }
  cursor = after;

  if (path == ".Touches") {
    for (auto const& slug : ctx.touches) {
      current_item item{.is_child = false, .text = slug, .external_key = {}};
      if (auto done = render_body(body, ctx, &item, out); !done.has_value()) {
        return done;
      }
    }
    return {};
  }
  if (path == ".Children") {
    for (auto const& child : ctx.children) {
      current_item item{.is_child = true, .text = child.title, .external_key = child.external_key};
      if (auto done = render_body(body, ctx, &item, out); !done.has_value()) {
        return done;
      }
    }
    return {};
  }
  return std::unexpected(render_error::unknown_field);
}

/// @brief Execute `{{if <path>}}`.
///
/// There is no `{{else}}` arm — `else` is rejected as an unsupported
/// directive before it can be reached, so a falsy test simply emits
/// nothing.
/// @param src The source text.
/// @param cursor Advanced past the matching `{{end}}`.
/// @param ctx The data.
/// @param item The current `range` binding, if any.
/// @param out The output buffer.
/// @param path The referenced field.
/// @return Nothing, or the first error.
auto exec_if(std::string_view src, std::size_t& cursor, const render_context& ctx, item_ref item, std::string& out,
             std::string_view path) -> std::expected<void, render_error> {
  std::string_view body;
  std::size_t      after = 0;
  if (auto found = find_matching_end(src, cursor, cursor, body, after); !found.has_value()) {
    return found;
  }
  cursor = after;

  auto const truthy = eval_truthy(ctx, item, path);
  if (!truthy.has_value()) {
    return std::unexpected(truthy.error());
  }
  if (!*truthy) {
    return {};
  }
  return render_body(body, ctx, item, out);
}

/// @brief Dispatch one directive's body (the text between `{{` and `}}`,
/// already trimmed).
/// @param src The source text.
/// @param cursor The cursor, positioned just past the directive.
/// @param ctx The data.
/// @param item The current `range` binding, if any.
/// @param out The output buffer.
/// @param directive The trimmed directive text.
/// @return Nothing, or the first error.
auto exec_directive(std::string_view src, std::size_t& cursor, const render_context& ctx, item_ref item, std::string& out,
                    std::string_view directive) -> std::expected<void, render_error> {
  if (directive.empty()) {
    return std::unexpected(render_error::unsupported_directive);
  }
  // A bare `end`/`else` here is a mismatched close — the matched ones are
  // consumed by `find_matching_end` and never reach this function.
  if (directive == "end" || directive == "else") {
    return std::unexpected(render_error::unsupported_directive);
  }
  if (directive.starts_with("range ")) {
    return exec_loop(src, cursor, ctx, out, trim_directive(directive.substr(6)));
  }
  if (directive.starts_with("if ")) {
    return exec_if(src, cursor, ctx, item, out, trim_directive(directive.substr(3)));
  }
  if (directive.front() != '.') {
    // `with`, a pipeline, a func call, a bare word — all refused here.
    return std::unexpected(render_error::unsupported_directive);
  }
  auto const value = resolve_reference(directive, ctx, item);
  if (!value.has_value()) {
    return std::unexpected(value.error());
  }
  out += *value;
  return {};
}

auto exec_range(std::string_view src, std::size_t& cursor, const render_context& ctx, item_ref item, std::string& out)
    -> std::expected<void, render_error> {
  while (cursor < src.size()) {
    auto const open = src.find("{{", cursor);
    if (open == std::string_view::npos) {
      out += src.substr(cursor);
      cursor = src.size();
      return {};
    }
    out += src.substr(cursor, open - cursor);
    auto const close = src.find("}}", open + 2);
    if (close == std::string_view::npos) {
      return std::unexpected(render_error::unclosed_directive);
    }
    auto const directive = trim_directive(src.substr(open + 2, close - (open + 2)));
    cursor               = close + 2;
    if (auto done = exec_directive(src, cursor, ctx, item, out, directive); !done.has_value()) {
      return done;
    }
  }
  return {};
}

} // namespace

auto error_name(render_error e) -> std::string_view {
  // These five strings are `@errorName` output from the Zig oracle and are
  // printed verbatim by `templates validate`. See render.cppm's header.
  switch (e) {
  case render_error::unsupported_directive:
    return "UnsupportedDirective";
  case render_error::unknown_field:
    return "UnknownField";
  case render_error::unclosed_directive:
    return "UnclosedDirective";
  case render_error::unexpected_end:
    return "UnexpectedEnd";
  case render_error::out_of_memory:
    return "OutOfMemory";
  }
  return "UnknownField";
}

auto exec_string(std::string_view src, const render_context& ctx) -> std::expected<std::string, render_error> {
  std::string out;
  std::size_t cursor = 0;
  // `item` is always absent at this entry point: the oracle's two callers
  // (`renderValue` and `validate`'s per-field walk) both pass null, and a
  // binding only exists inside a `range` body, which `exec_loop` creates.
  if (auto done = exec_range(src, cursor, ctx, nullptr, out); !done.has_value()) {
    return std::unexpected(done.error());
  }
  return out;
}

auto render_template(const json_value& fields, const render_context& ctx) -> std::expected<json_value, render_error> {
  switch (fields.kind) {
  case json_kind::string: {
    auto rendered = exec_string(fields.string, ctx);
    if (!rendered.has_value()) {
      return std::unexpected(rendered.error());
    }
    return json_value{.kind = json_kind::string, .string = std::move(*rendered)};
  }
  case json_kind::object: {
    json_value out{.kind = json_kind::object};
    out.object.reserve(fields.object.size());
    for (auto const& [key, value] : fields.object) {
      auto child = render_template(value, ctx);
      if (!child.has_value()) {
        return std::unexpected(child.error());
      }
      // The KEY is copied verbatim, never rendered — the oracle's
      // `renderValue` dupes it and only recurses into the value.
      out.object.emplace_back(key, std::move(*child));
    }
    return out;
  }
  case json_kind::array: {
    json_value out{.kind = json_kind::array};
    out.array.reserve(fields.array.size());
    for (auto const& item : fields.array) {
      auto child = render_template(item, ctx);
      if (!child.has_value()) {
        return std::unexpected(child.error());
      }
      out.array.push_back(std::move(*child));
    }
    return out;
  }
  default:
    // Numbers, booleans and nulls pass through untouched.
    return fields;
  }
}

} // namespace planar::engine::templates
