/// @file greedy.cpp
/// @brief Implementation of `planar.engine.grouping.greedy` (plan 996, task
/// 6095). See greedy.cppm for the algorithm's contract and the oracle-derived
/// tie-break, D-HG1, and budget semantics.

module planar.engine.grouping.greedy;

import std;

namespace planar::engine::grouping::greedy {

namespace {

/// @brief A slice under construction.
///
/// `union_w` and `writers` are kept current after every merge so the budget
/// check is EXACT by construction rather than approximated by a balance
/// metric. `dead` tombstones a slice that has been absorbed; indices stay
/// stable because the cycle check addresses slices by index.
struct work_slice {
  std::vector<std::int64_t>                      members;
  std::unordered_map<std::string, std::uint32_t> union_w; ///< symbol -> weight.
  std::unordered_set<std::string>                writers; ///< symbols some member modifies.
  std::uint32_t                                  cost = 0;
  bool                                           dead = false;
};

/// @brief A candidate merge, with its score and the two tie-break keys.
struct candidate {
  std::size_t   lo      = 0; ///< Index of the slice that survives.
  std::size_t   hi      = 0; ///< Index of the slice that is absorbed.
  std::uint64_t score   = 0; ///< Overlap score (write-write pairs excluded).
  std::int64_t  tie_min = 0; ///< Smallest member id across the two slices.
  std::int64_t  tie_max = 0; ///< The larger of the two slices' smallest members.
};

auto add_unit(work_slice& ws, const unit& u) -> void {
  const auto it = ws.union_w.find(u.qualified);
  if (it == ws.union_w.end()) {
    ws.union_w.emplace(u.qualified, u.weight);
    ws.cost += u.weight;
  } else if (u.weight > it->second) {
    // Same symbol, larger weight seen. Defensive: a symbol's weight is a
    // fixed property of the source so every task's copy should agree, but the
    // running cost stays exact either way.
    ws.cost += u.weight - it->second;
    it->second = u.weight;
  }
  if (u.role_ == role::modify) {
    ws.writers.insert(u.qualified);
  }
}

/// @brief Overlap score: the weight of every symbol present in BOTH unions,
/// EXCEPT one both slices modify, which contributes zero (D-HG1).
auto overlap_score(const work_slice& x, const work_slice& y) -> std::uint64_t {
  std::uint64_t score = 0;
  for (const auto& [sym, wx] : x.union_w) {
    const auto it = y.union_w.find(sym);
    if (it == y.union_w.end()) {
      continue;
    }
    if (x.writers.contains(sym) && y.writers.contains(sym)) {
      continue; // write-write conflict: no credit.
    }
    score += std::max(wx, it->second);
  }
  return score;
}

/// @brief Cost of the union of two slices' closures, deduplicating shared
/// symbols so each is paid for once.
auto union_cost(const work_slice& x, const work_slice& y) -> std::uint32_t {
  std::uint32_t total = x.cost;
  for (const auto& [sym, w] : y.union_w) {
    if (!x.union_w.contains(sym)) {
      total += w;
    }
  }
  return total;
}

auto min_member(const work_slice& ws) -> std::int64_t {
  return *std::ranges::min_element(ws.members);
}

auto member_of(const work_slice& ws, std::int64_t id) -> bool {
  return std::ranges::find(ws.members, id) != ws.members.end();
}

/// @brief True if any dependency edge connects a member of `x` to a member of
/// `y`, in either direction.
auto pair_has_dep(const work_slice& x, const work_slice& y, std::span<const dep> deps) -> bool {
  for (const auto& d : deps) {
    const bool x_blocked = member_of(x, d.blocked);
    const bool x_blocker = member_of(x, d.blocker);
    const bool y_blocked = member_of(y, d.blocked);
    const bool y_blocker = member_of(y, d.blocker);
    if ((x_blocked && y_blocker) || (x_blocker && y_blocked)) {
      return true;
    }
  }
  return false;
}

auto slice_of_task(std::span<const work_slice> slices, std::int64_t id) -> std::optional<std::size_t> {
  for (std::size_t k = 0; k < slices.size(); ++k) {
    if (slices[k].dead) {
      continue;
    }
    if (member_of(slices[k], id)) {
      return k;
    }
  }
  return std::nullopt;
}

using adjacency = std::unordered_map<std::size_t, std::unordered_set<std::size_t>>;

/// @brief Three-colour DFS cycle detection over the contracted slice graph.
auto has_cycle_from(const adjacency& adj, std::unordered_map<std::size_t, int>& colour, std::size_t node) -> bool {
  colour[node] = 1; // grey
  if (const auto it = adj.find(node); it != adj.end()) {
    for (const std::size_t nb : it->second) {
      const auto c = colour.contains(nb) ? colour.at(nb) : 0;
      if (c == 1) {
        return true; // back edge
      }
      if (c == 0 && has_cycle_from(adj, colour, nb)) {
        return true;
      }
    }
  }
  colour[node] = 2; // black
  return false;
}

/// @brief True iff merging slices `mi` and `mj` would induce a cycle in the
/// slice-level precedence DAG.
///
/// Builds the POST-merge graph (treating mi/mj as one node) and runs a cycle
/// check on it directly. Exact rather than heuristic: ordering wins over
/// replication, so any doubt rejects the merge.
auto merge_induces_cycle(std::span<const work_slice> slices, std::span<const dep> deps, std::size_t mi, std::size_t mj) -> bool {
  std::vector<std::size_t> node_of(slices.size(), std::numeric_limits<std::size_t>::max());
  for (std::size_t k = 0; k < slices.size(); ++k) {
    if (slices[k].dead) {
      continue;
    }
    node_of[k] = (k == mj) ? mi : k;
  }

  adjacency adj;
  for (const auto& d : deps) {
    const auto sb = slice_of_task(slices, d.blocker);
    const auto st = slice_of_task(slices, d.blocked);
    if (!sb.has_value() || !st.has_value()) {
      continue;
    }
    const auto nb = node_of[*sb];
    const auto nt = node_of[*st];
    if (nb == nt) {
      continue; // intra-node edge contributes no slice-DAG edge.
    }
    adj[nb].insert(nt);
  }

  std::unordered_map<std::size_t, int> colour;
  std::vector<std::size_t>             roots;
  roots.reserve(adj.size());
  for (const auto& [node, _] : adj) {
    roots.push_back(node);
  }
  // Sorted so the traversal order is deterministic. The ANSWER does not depend
  // on it (a cycle is found from any root that reaches it), but a deterministic
  // walk keeps failures reproducible.
  std::ranges::sort(roots);
  for (const std::size_t r : roots) {
    const auto c = colour.contains(r) ? colour.at(r) : 0;
    if (c == 0 && has_cycle_from(adj, colour, r)) {
      return true;
    }
  }
  return false;
}

/// @brief Strict "is `x` a better pick than `y`": higher score wins, then the
/// smaller `tie_min`, then the smaller `tie_max`. A total order, so the merge
/// sequence is stable.
auto candidate_better(const candidate& x, const candidate& y) -> bool {
  if (x.score != y.score) {
    return x.score > y.score;
  }
  if (x.tie_min != y.tie_min) {
    return x.tie_min < y.tie_min;
  }
  return x.tie_max < y.tie_max;
}

auto best_merge(std::span<const work_slice> slices, std::span<const dep> deps, std::uint32_t budget) -> std::optional<candidate> {
  std::optional<candidate> best;
  for (std::size_t i = 0; i < slices.size(); ++i) {
    if (slices[i].dead) {
      continue;
    }
    for (std::size_t j = i + 1; j < slices.size(); ++j) {
      if (slices[j].dead) {
        continue;
      }
      // Budget: the merged union cost, computed directly with shared symbols
      // deduplicated, so the constraint is exact.
      if (union_cost(slices[i], slices[j]) > budget) {
        continue;
      }
      if (merge_induces_cycle(slices, deps, i, j)) {
        continue;
      }

      const auto score = overlap_score(slices[i], slices[j]);
      // A zero-score merge is permitted only along a dependency: fusing two
      // genuinely unrelated tasks would inflate a slice's cost for nothing.
      if (score == 0 && !pair_has_dep(slices[i], slices[j], deps)) {
        continue;
      }

      const auto      lo_min = min_member(slices[i]);
      const auto      hi_min = min_member(slices[j]);
      const candidate cand{
          .lo      = i,
          .hi      = j,
          .score   = score,
          .tie_min = std::min(lo_min, hi_min),
          .tie_max = std::max(lo_min, hi_min),
      };
      if (!best.has_value() || candidate_better(cand, *best)) {
        best = cand;
      }
    }
  }
  return best;
}

auto merge_into(work_slice& dst, const work_slice& src) -> void {
  dst.members.insert(dst.members.end(), src.members.begin(), src.members.end());
  for (const auto& [sym, w] : src.union_w) {
    const auto it = dst.union_w.find(sym);
    if (it == dst.union_w.end()) {
      dst.union_w.emplace(sym, w);
      dst.cost += w;
    } else if (w > it->second) {
      dst.cost += w - it->second;
      it->second = w;
    }
  }
  for (const auto& sym : src.writers) {
    dst.writers.insert(sym);
  }
}

auto finalize_slice(const work_slice& ws) -> slice {
  slice out;
  out.task_ids = ws.members;
  std::ranges::sort(out.task_ids);
  out.union_symbols.reserve(ws.union_w.size());
  for (const auto& [sym, _] : ws.union_w) {
    out.union_symbols.push_back(sym);
  }
  // The union comes out of a hash map, so this sort is what makes the emitted
  // symbol list deterministic at all.
  std::ranges::sort(out.union_symbols);
  out.cost = ws.cost;
  return out;
}

} // namespace

auto grouping::total_cost() const -> std::uint64_t {
  std::uint64_t total = 0;
  for (const auto& s : slices) {
    total += s.cost;
  }
  return total;
}

auto group(std::span<const task> tasks, std::span<const dep> deps, std::uint32_t budget) -> grouping {
  // --- 1. Seed one slice per task ---------------------------------------
  std::vector<work_slice> slices(tasks.size());
  for (std::size_t i = 0; i < tasks.size(); ++i) {
    slices[i].members.push_back(tasks[i].id);
    for (const auto& u : tasks[i].units) {
      add_unit(slices[i], u);
    }
  }

  // --- 2. Greedy merge rounds -------------------------------------------
  while (true) {
    const auto best = best_merge(slices, deps, budget);
    if (!best.has_value()) {
      break;
    }
    merge_into(slices[best->lo], slices[best->hi]);
    slices[best->hi].dead = true;
  }

  // --- 3. Materialize ----------------------------------------------------
  grouping out;
  for (const auto& ws : slices) {
    if (ws.dead) {
      continue;
    }
    out.slices.push_back(finalize_slice(ws));
  }
  // Deterministic slice ordering: by smallest member id. Every slice has at
  // least one member, and members were sorted in finalize_slice.
  std::ranges::sort(out.slices, [](const slice& a, const slice& b) { return a.task_ids.front() < b.task_ids.front(); });
  return out;
}

} // namespace planar::engine::grouping::greedy
