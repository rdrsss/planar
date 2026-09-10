/// @file mtkahypar.cpp
/// @brief Implementation of `planar.engine.grouping.mtkahypar` (see
/// mtkahypar.cppm for the seam's contract and the D-HG1 translation rule).
///
/// The vendored `mtkahypar.h` C ABI is confined to this translation unit's
/// global module fragment, gated entirely behind `PLANAR_HAS_MTKAHYPAR`
/// (defined by this bucket's `CMakeLists.txt` ONLY when
/// `-DPLANAR_WITH_MTKAHYPAR=ON` AND the `MtKaHyPar::mtkahypar` target
/// exists). When the macro is undefined this whole file compiles to the two
/// exported functions returning the "unavailable" answer and references no
/// `mt_kahypar_*` symbol at all — no other file in the module purview names
/// a raw Mt-KaHyPar type either way.

module;

#if defined(PLANAR_HAS_MTKAHYPAR)
#include <mtkahypar.h>
#endif

module planar.engine.grouping.mtkahypar;

import std;
import planar.engine.grouping.greedy;

namespace planar::engine::grouping::mtkahypar {

#if defined(PLANAR_HAS_MTKAHYPAR)

namespace {

/// @brief `mt_kahypar_initialize` must run exactly once per process, before
/// the first hypergraph is built. Guarded so repeated `try_partition` calls
/// (e.g. across multiple `groups recommend` invocations in one process, as
/// the test binary does) do not re-initialize the thread pool.
auto ensure_initialized() -> void {
  static const bool once = [] {
    const auto hw = std::thread::hardware_concurrency();
    mt_kahypar_initialize(hw == 0 ? 1 : hw, /*interleaved_allocations=*/true);
    return true;
  }();
  (void)once;
}

/// @brief `ceil(deduped_union / budget)`, clamped to at least 1; `budget==0`
/// is defined as 1 (mirrors the oracle's `mtkahypar.blockCount`, task 4247's
/// fix: the deduped union of ALL tasks' closures, not the sum of each task's
/// own closure cost, which would over-count for tightly-coupled tasks and
/// force a `k` too large to let the solver merge them).
auto block_count(std::span<const greedy::task> tasks, std::uint32_t budget) -> std::uint32_t {
  if (budget == 0) {
    return 1;
  }
  std::unordered_map<std::string, std::uint32_t> seen;
  std::uint64_t                                  total = 0;
  for (const auto& t : tasks) {
    for (const auto& u : t.units) {
      const auto it = seen.find(u.qualified);
      if (it == seen.end()) {
        seen.emplace(u.qualified, u.weight);
        total += u.weight;
      } else if (u.weight > it->second) {
        total += u.weight - it->second;
        it->second = u.weight;
      }
    }
  }
  if (total == 0) {
    return 1;
  }
  const auto k = (total + budget - 1) / budget;
  return static_cast<std::uint32_t>(std::max<std::uint64_t>(1, k));
}

/// @brief The pure, allocation-only hypergraph translation. See
/// mtkahypar.cppm's D-HG1 section for the write-write zero-credit rule.
///
/// Vertices are tasks, 0-based, ordered by ascending `task.id` — the C API
/// (unlike the hMETIS TEXT format the Zig oracle wrote to a temp file) takes
/// 0-based ids directly, so no vertex-numbering offset is needed.
struct built_hypergraph {
  std::vector<std::int64_t>                  vertex_id_of; ///< index -> task.id, ascending.
  std::vector<mt_kahypar_hypernode_weight_t> vertex_weights;
  std::vector<std::size_t>                   hyperedge_indices; ///< CSR offsets, size = num_edges + 1.
  std::vector<mt_kahypar_hypernode_id_t>     pins;
  std::vector<mt_kahypar_hyperedge_weight_t> hyperedge_weights;
};

auto build_hypergraph(std::span<const greedy::task> tasks) -> built_hypergraph {
  built_hypergraph out;
  out.vertex_id_of.reserve(tasks.size());
  out.vertex_weights.reserve(tasks.size());

  // Tasks are already loaded in ascending id order by
  // `load::load_open_task_ids` (ORDER BY priority, id) -- NOT necessarily
  // ascending by id. Sort a local index so vertex numbering is deterministic
  // and matches the "ascending task.id" contract documented in the header.
  std::vector<std::size_t> order(tasks.size());
  std::iota(order.begin(), order.end(), 0);
  std::ranges::sort(order, [&](std::size_t a, std::size_t b) { return tasks[a].id < tasks[b].id; });

  std::unordered_map<std::int64_t, mt_kahypar_hypernode_id_t> vertex_of_task;
  vertex_of_task.reserve(tasks.size());
  for (const auto idx : order) {
    const auto v = static_cast<mt_kahypar_hypernode_id_t>(out.vertex_id_of.size());
    vertex_of_task.emplace(tasks[idx].id, v);
    out.vertex_id_of.push_back(tasks[idx].id);
    mt_kahypar_hypernode_weight_t   weight = 0;
    std::unordered_set<std::string> dedup;
    for (const auto& u : tasks[idx].units) {
      if (dedup.insert(u.qualified).second) {
        weight += static_cast<mt_kahypar_hypernode_weight_t>(u.weight);
      }
    }
    out.vertex_weights.push_back(weight);
  }

  // Per-symbol: which tasks hold it, and under which roles.
  struct holder {
    std::vector<mt_kahypar_hypernode_id_t> pins; // ascending vertex id
    std::uint32_t                          weight  = 0;
    std::uint32_t                          writers = 0;
  };
  std::map<std::string, holder> by_symbol; // std::map: deterministic (sorted) iteration order.
  for (const auto idx : order) {
    for (const auto& u : tasks[idx].units) {
      auto& h = by_symbol[u.qualified];
      h.pins.push_back(vertex_of_task.at(tasks[idx].id));
      h.weight = std::max(h.weight, u.weight);
      if (u.role_ == greedy::role::modify) {
        ++h.writers;
      }
    }
  }

  out.hyperedge_indices.push_back(0);
  for (const auto& [symbol, h] : by_symbol) {
    if (h.pins.size() < 2) {
      continue; // a single pin can never be cut -- no information.
    }
    if (h.writers >= 2) {
      continue; // D-HG1: two-or-more writers on one symbol earn ZERO credit.
    }
    auto pins = h.pins;
    std::ranges::sort(pins);
    out.pins.insert(out.pins.end(), pins.begin(), pins.end());
    out.hyperedge_indices.push_back(out.pins.size());
    out.hyperedge_weights.push_back(static_cast<mt_kahypar_hyperedge_weight_t>(h.weight));
  }
  return out;
}

} // namespace

auto available() -> bool {
  return true;
}

auto try_partition(std::span<const greedy::task> tasks, std::span<const greedy::dep>, std::uint32_t budget)
    -> std::optional<greedy::grouping> {
  if (tasks.empty()) {
    return greedy::grouping{};
  }

  const auto k        = block_count(tasks, budget);
  auto       hg_input = build_hypergraph(tasks);

  // k == 1: the deduped union already fits the budget by construction of
  // block_count, so the single-block answer (everything merged) is trivially
  // valid and there is nothing for the solver to decide.
  if (k <= 1) {
    greedy::slice s;
    s.task_ids = hg_input.vertex_id_of;
    std::ranges::sort(s.task_ids);
    std::unordered_map<std::string, std::uint32_t> union_w;
    for (const auto& t : tasks) {
      for (const auto& u : t.units) {
        auto& w = union_w[u.qualified];
        w       = std::max(w, u.weight);
      }
    }
    for (const auto& [sym, w] : union_w) {
      s.union_symbols.push_back(sym);
      s.cost += w;
    }
    std::ranges::sort(s.union_symbols);
    greedy::grouping out;
    out.slices.push_back(std::move(s));
    return out;
  }

  ensure_initialized();

  mt_kahypar_context_t* context = mt_kahypar_context_from_preset(DETERMINISTIC);
  if (context == nullptr) {
    return std::nullopt;
  }
  mt_kahypar_set_partitioning_parameters(context, static_cast<mt_kahypar_partition_id_t>(k), 0.03, KM1);
  mt_kahypar_set_seed(42); // Deterministic across runs, matching greedy's own determinism contract.

  mt_kahypar_error_t error{};
  const auto         num_vertices   = static_cast<mt_kahypar_hypernode_id_t>(hg_input.vertex_id_of.size());
  const auto         num_hyperedges = static_cast<mt_kahypar_hyperedge_id_t>(hg_input.hyperedge_weights.size());

  const mt_kahypar_hypergraph_t hypergraph = mt_kahypar_create_hypergraph(
      context, num_vertices, num_hyperedges, hg_input.hyperedge_indices.data(),
      num_hyperedges == 0 ? nullptr : hg_input.pins.data(), num_hyperedges == 0 ? nullptr : hg_input.hyperedge_weights.data(),
      hg_input.vertex_weights.data(), &error);
  if (error.status != SUCCESS) {
    mt_kahypar_free_error_content(&error);
    mt_kahypar_free_context(context);
    return std::nullopt;
  }

  mt_kahypar_error_t                        partition_error{};
  const mt_kahypar_partitioned_hypergraph_t partitioned = mt_kahypar_partition(hypergraph, context, &partition_error);
  if (partition_error.status != SUCCESS) {
    mt_kahypar_free_error_content(&partition_error);
    mt_kahypar_free_hypergraph(hypergraph);
    mt_kahypar_free_context(context);
    return std::nullopt;
  }

  std::vector<mt_kahypar_partition_id_t> block_of(num_vertices);
  mt_kahypar_get_partition(partitioned, block_of.data());

  std::map<mt_kahypar_partition_id_t, std::vector<std::size_t>> members_by_block; // vertex indices
  for (mt_kahypar_hypernode_id_t v = 0; v < num_vertices; ++v) {
    members_by_block[block_of[v]].push_back(v);
  }

  greedy::grouping out;
  for (const auto& [block, verts] : members_by_block) {
    greedy::slice                                  s;
    std::unordered_map<std::string, std::uint32_t> union_w;
    for (const auto vi : verts) {
      s.task_ids.push_back(hg_input.vertex_id_of[vi]);
      const auto task_idx = std::ranges::find(tasks, hg_input.vertex_id_of[vi], &greedy::task::id) - tasks.begin();
      for (const auto& u : tasks[static_cast<std::size_t>(task_idx)].units) {
        auto& w = union_w[u.qualified];
        w       = std::max(w, u.weight);
      }
    }
    std::ranges::sort(s.task_ids);
    for (const auto& [sym, w] : union_w) {
      s.union_symbols.push_back(sym);
      s.cost += w;
    }
    std::ranges::sort(s.union_symbols);
    out.slices.push_back(std::move(s));
  }
  std::ranges::sort(out.slices,
                    [](const greedy::slice& a, const greedy::slice& b) { return a.task_ids.front() < b.task_ids.front(); });

  mt_kahypar_free_partitioned_hypergraph(partitioned);
  mt_kahypar_free_hypergraph(hypergraph);
  mt_kahypar_free_context(context);
  return out;
}

#else // !defined(PLANAR_HAS_MTKAHYPAR)

auto available() -> bool {
  return false;
}

auto try_partition(std::span<const greedy::task>, std::span<const greedy::dep>, std::uint32_t)
    -> std::optional<greedy::grouping> {
  return std::nullopt;
}

#endif

} // namespace planar::engine::grouping::mtkahypar
