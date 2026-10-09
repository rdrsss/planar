/// @file tui.cpp
/// @brief Implementation of `planar.cmd.planar_watch.handlers.agents.tui`.
module;
#include <ftxui/component/component.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <unistd.h>
module planar.cmd.planar_watch.handlers.agents.tui;
import std;
import planar.cmd.planar_watch.context;
import planar.cmd.planar_watch.exit;
import planar.cmd.planar_watch.handlers.agents.model;

namespace planar::cmd::watch::agents {

namespace {

/// How long a stopped agent stays listed.
constexpr std::int64_t k_stopped_window_secs = 3600;

/// Time between refreshes.
constexpr auto k_refresh_interval = std::chrono::milliseconds{1000};

auto state_color(agent_state state) -> ftxui::Color {
  switch (state) {
  case agent_state::working:
    return ftxui::Color::Green;
  case agent_state::waiting:
    return ftxui::Color::Yellow;
  case agent_state::stopped:
    return ftxui::Color::Red;
  }
  return ftxui::Color::Default;
}

auto task_glyph(std::string_view status) -> std::string {
  if (status == "done")
    return "✓";
  if (status == "doing")
    return "◐";
  if (status == "blocked")
    return "⊘";
  if (status == "cancelled")
    return "✗";
  return "·";
}

auto draw_row(const row& r, bool selected, ftxui::Box& box) -> ftxui::Element {
  using namespace ftxui;
  Elements parts;
  parts.push_back(text(std::string(static_cast<std::size_t>(r.depth) * 2, ' ')));

  // The caret column: `>` in the holder's colour on a held task.
  if (r.caret.has_value())
    parts.push_back(text("> ") | bold | color(state_color(*r.caret)));
  else
    parts.push_back(text("  "));

  if (r.expandable)
    parts.push_back(text(r.expanded ? "▾ " : "▸ "));
  else
    parts.push_back(text("  "));

  if (r.dot.has_value())
    parts.push_back(text("● ") | color(state_color(*r.dot)));
  if (r.kind == row_kind::task) {
    auto glyph = text(task_glyph(r.status) + " ");
    if (r.status == "done")
      glyph = glyph | color(Color::Green);
    parts.push_back(glyph);
  }

  // Finished work stays listed, in dark grey.
  auto label = text(r.text);
  if (r.kind == row_kind::agent)
    label = label | bold;
  if (r.dim)
    label = label | color(Color::GrayDark);
  // The columns to the left keep their width; a long label or detail is
  // clipped at the screen edge instead of squeezing them.
  // An agent row keeps its name whole and lets the status text give way.
  Elements line_parts{hbox(std::move(parts)), r.kind == row_kind::agent ? label : label | xflex_shrink};
  if (!r.detail.empty())
    line_parts.push_back(text("  " + r.detail) | dim | xflex_shrink);

  auto line = hbox(std::move(line_parts));
  if (selected)
    line = line | inverted | focus;
  // Where the row landed on screen, for mouse hit-testing. A row scrolled
  // out of the frame gets an empty box.
  return line | reflect(box);
}

auto error_text(const domain_error& err) -> std::string {
  std::ostringstream out;
  report(err, out);
  auto text = out.str();
  while (!text.empty() && text.back() == '\n')
    text.pop_back();
  return text;
}

/// The interactive session's state. Touched only on the UI thread.
struct session {
  context&                ctx;
  snapshot                snap;
  view_state              view;
  std::vector<row>        rows;
  std::vector<ftxui::Box> row_boxes; ///< Each row's screen area from the last frame.
  std::size_t             selected = 0;
  std::string             selected_key;
  std::string             error;

  auto reload() -> void {
    auto conn = ctx.db().ensure_db();
    if (!conn) {
      error = error_text(conn.error());
      return;
    }
    auto const now  = now_timestamp();
    auto       next = build_snapshot(**conn, options{.now = now, .stopped_since = timestamp_minus(now, k_stopped_window_secs)});
    if (!next) {
      error = std::format("error: reading agents: {}", next.error().message_);
      return;
    }
    error.clear();
    snap = std::move(*next);
    relayout();
  }

  /// Rebuild the rows and keep the selection on the same key when it still exists.
  auto relayout() -> void {
    rows = flatten(snap, view);
    if (rows.empty()) {
      selected = 0;
      return;
    }
    if (!selected_key.empty()) {
      for (std::size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].key == selected_key) {
          selected = i;
          return;
        }
      }
    }
    selected     = std::min(selected, rows.size() - 1);
    selected_key = rows[selected].key;
  }

  auto select(std::size_t index) -> void {
    if (rows.empty())
      return;
    selected     = std::min(index, rows.size() - 1);
    selected_key = rows[selected].key;
  }

  auto toggle(const row& r) -> void {
    if (!view.toggled.erase(r.key))
      view.toggled.insert(r.key);
    relayout();
  }

  /// Right: open a closed row, or step into an open one.
  auto expand() -> void {
    if (rows.empty())
      return;
    auto const& r = rows[selected];
    if (r.expandable && !r.expanded)
      toggle(r);
    else if (r.expandable)
      select(selected + 1);
  }

  /// Left: close an open row, or step out to the parent.
  auto collapse() -> void {
    if (rows.empty())
      return;
    auto const& r = rows[selected];
    if (r.expandable && r.expanded) {
      toggle(r);
      return;
    }
    for (std::size_t i = selected; i-- > 0;) {
      if (rows[i].depth < r.depth) {
        select(i);
        return;
      }
    }
  }

  auto render() -> ftxui::Element {
    using namespace ftxui;
    int working = 0, waiting = 0, stopped = 0;
    for (auto const& [id, claim] : snap.claims) {
      if (claim.entity_kind == "task" && claim.status != "active")
        continue;
      switch (claim.state) {
      case agent_state::working:
        ++working;
        break;
      case agent_state::waiting:
        ++waiting;
        break;
      case agent_state::stopped:
        ++stopped;
        break;
      }
    }
    auto header = hbox({
        text(" planar-watch ") | bold,
        text("agents  "),
        text("● ") | color(Color::Green),
        text(std::format("{} working  ", working)),
        text("● ") | color(Color::Yellow),
        text(std::format("{} waiting  ", waiting)),
        text("● ") | color(Color::Red),
        text(std::format("{} stopped", stopped)),
        filler(),
        text(snap.generated_at.size() >= 19 ? snap.generated_at.substr(11, 8) + " UTC " : std::string{}) | dim,
    });

    Elements lines;
    if (rows.empty())
      lines.push_back(text(view.show_stopped ? "  no agents in the last hour" : "  no live agents") | dim);
    row_boxes.assign(rows.size(), ftxui::Box{});
    for (std::size_t i = 0; i < rows.size(); ++i)
      lines.push_back(draw_row(rows[i], i == selected, row_boxes[i]));

    auto footer = error.empty() ? text(std::format(" ↑↓ move  ←→ fold  enter/click toggle  s {} stopped  q quit",
                                                   view.show_stopped ? "hide" : "show")) |
                                      dim
                                : text(" " + error) | color(Color::Red);
    return vbox({header, separator(), vbox(std::move(lines)) | vscroll_indicator | yframe | flex, separator(), footer});
  }

  /// Left click selects a row and toggles it when it folds; the wheel moves the selection.
  auto on_mouse(const ftxui::Mouse& mouse) -> bool {
    using ftxui::Mouse;
    if (mouse.button == Mouse::WheelDown) {
      select(selected + 1);
      return true;
    }
    if (mouse.button == Mouse::WheelUp) {
      if (selected > 0)
        select(selected - 1);
      return true;
    }
    if (mouse.button != Mouse::Left || mouse.motion != Mouse::Pressed)
      return false;
    for (std::size_t i = 0; i < row_boxes.size() && i < rows.size(); ++i) {
      if (!row_boxes[i].Contain(mouse.x, mouse.y))
        continue;
      select(i);
      if (rows[i].expandable)
        toggle(rows[i]);
      return true;
    }
    return false;
  }

  auto on_event(const ftxui::Event& event, ftxui::ScreenInteractive& screen) -> bool {
    using ftxui::Event;
    if (event == Event::Character('q') || event == Event::Escape) {
      screen.Exit();
      return true;
    }
    if (event == Event::Custom) {
      reload();
      return true;
    }
    if (event.is_mouse())
      return on_mouse(ftxui::Event{event}.mouse()); // FTXUI's `mouse()` is non-const
    if (event == Event::ArrowDown || event == Event::Character('j')) {
      select(selected + 1);
      return true;
    }
    if (event == Event::ArrowUp || event == Event::Character('k')) {
      if (selected > 0)
        select(selected - 1);
      return true;
    }
    if (event == Event::Home || event == Event::Character('g')) {
      select(0);
      return true;
    }
    if (event == Event::End || event == Event::Character('G')) {
      if (!rows.empty())
        select(rows.size() - 1);
      return true;
    }
    if (event == Event::ArrowRight || event == Event::Character('l')) {
      expand();
      return true;
    }
    if (event == Event::ArrowLeft || event == Event::Character('h')) {
      collapse();
      return true;
    }
    if (event == Event::Return || event == Event::Character(' ')) {
      if (!rows.empty() && rows[selected].expandable)
        toggle(rows[selected]);
      return true;
    }
    if (event == Event::Character('s')) {
      view.show_stopped = !view.show_stopped;
      relayout();
      return true;
    }
    if (event == Event::Character('r')) {
      reload();
      return true;
    }
    return false;
  }
};

} // namespace

auto wants_interactive(std::span<const std::string> argv, bool stdin_tty, bool stdout_tty, const env_lookup& env) -> bool {
  if (argv.size() > 1 || !stdin_tty || !stdout_tty)
    return false;
  auto const term = env("TERM");
  return !term.has_value() || (!term->empty() && *term != "dumb");
}

auto stdin_is_tty() -> bool {
  return ::isatty(STDIN_FILENO) == 1;
}

auto stdout_is_tty(std::ostream& out) -> bool {
  return &out == &std::cout && ::isatty(STDOUT_FILENO) == 1;
}

auto run_interactive(context& ctx) -> int {
  session state{.ctx = ctx, .snap = {}, .view = {}, .rows = {}, .selected = 0, .selected_key = {}, .error = {}};
  state.reload();
  if (!state.error.empty() && state.snap.generated_at.empty()) {
    // Nothing to show at all: report the failure the way the other verbs do.
    auto conn = ctx.db().ensure_db();
    if (!conn) {
      report(conn.error(), ctx.err());
      return exit_code(conn.error());
    }
    ctx.err() << state.error << '\n';
    return exit_generic_failure;
  }

  auto screen   = ftxui::ScreenInteractive::Fullscreen();
  auto renderer = ftxui::Renderer([&] { return state.render(); });
  auto root     = ftxui::CatchEvent(renderer, [&](const ftxui::Event& event) { return state.on_event(event, screen); });

  // The ticker only posts an event; every database read happens on the UI thread.
  std::jthread ticker([&screen](std::stop_token stop) {
    while (!stop.stop_requested()) {
      for (auto waited = std::chrono::milliseconds{0}; waited < k_refresh_interval && !stop.stop_requested();
           waited += std::chrono::milliseconds{100})
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
      if (!stop.stop_requested())
        screen.PostEvent(ftxui::Event::Custom);
    }
  });
  screen.Loop(root);
  ticker.request_stop();
  ticker.join();
  return exit_success;
}

} // namespace planar::cmd::watch::agents
