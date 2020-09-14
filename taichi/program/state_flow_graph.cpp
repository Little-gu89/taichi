#include "taichi/program/state_flow_graph.h"

#include "taichi/ir/transforms.h"
#include "taichi/program/async_engine.h"
#include "taichi/util/bit.h"
#include "state_flow_graph.h"

#include <sstream>
#include <unordered_set>

TLANG_NAMESPACE_BEGIN

// TODO: rename state to edge since we have not only state flow edges but also
// dependency edges.

std::string StateFlowGraph::Node::string() const {
  return fmt::format("[node: {}:{}]", meta->name, launch_id);
}

StateFlowGraph::StateFlowGraph(IRBank *ir_bank) : ir_bank_(ir_bank) {
  nodes_.push_back(std::make_unique<Node>());
  initial_node_ = nodes_.back().get();
  initial_meta_.name = "initial_state";
  initial_node_->meta = &initial_meta_;
  initial_node_->launch_id = 0;
  initial_node_->is_initial_node = true;
}

void StateFlowGraph::clear() {
  // TODO: GC here?
  nodes_.resize(1);  // Erase all nodes except the initial one
  initial_node_->output_edges.clear();
  latest_state_owner_.clear();
  latest_state_readers_.clear();

  // Do not clear task_name_to_launch_ids_.
}

void StateFlowGraph::insert_task(const TaskLaunchRecord &rec) {
  auto node = std::make_unique<Node>();
  node->rec = rec;
  node->meta = get_task_meta(ir_bank_, rec);
  {
    int &id = task_name_to_launch_ids_[node->meta->name];
    node->launch_id = id;
    ++id;
  }
  for (auto input_state : node->meta->input_states) {
    if (latest_state_owner_.find(input_state) == latest_state_owner_.end()) {
      latest_state_owner_[input_state] = initial_node_;
    }
    insert_state_flow(latest_state_owner_[input_state], node.get(),
                      input_state);
  }
  for (auto output_state : node->meta->output_states) {
    latest_state_owner_[output_state] = node.get();
    if (latest_state_readers_.find(output_state) ==
        latest_state_readers_.end()) {
      latest_state_readers_[output_state].insert(initial_node_);
    }
    for (auto &d : latest_state_readers_[output_state]) {
      // insert a dependency edge
      insert_state_flow(d, node.get(), output_state);
    }
    latest_state_readers_[output_state].clear();
  }

  // Note that this loop must happen AFTER the previous one
  for (auto input_state : node->meta->input_states) {
    latest_state_readers_[input_state].insert(node.get());
  }
  nodes_.push_back(std::move(node));
}

void StateFlowGraph::insert_state_flow(Node *from, Node *to, AsyncState state) {
  TI_ASSERT(from != nullptr);
  TI_ASSERT(to != nullptr);
  from->output_edges[state].insert(to);
  to->input_edges[state].insert(from);
}

bool StateFlowGraph::optimize_listgen() {
  TI_INFO("Begin optimize listgen");
  bool modified = false;

  using bit::Bitset;
  const int n = nodes_.size();

  // Compute the transitive closure.
  auto has_path = std::make_unique<Bitset[]>(n);
  auto has_path_reverse = std::make_unique<Bitset[]>(n);
  // has_path[i][j] denotes if there is a path from i to j.
  // has_path_reverse[i][j] denotes if there is a path from j to i.
  for (int i = 0; i < n; i++) {
    has_path[i] = Bitset(n);
    has_path[i][i] = true;
    has_path_reverse[i] = Bitset(n);
    has_path_reverse[i][i] = true;
  }
  for (int i = n - 1; i >= 0; i--) {
    for (auto &edges : nodes_[i]->input_edges) {
      for (auto &edge : edges.second) {
        TI_ASSERT(edge->node_id < i);
        has_path[edge->node_id] |= has_path[i];
      }
    }
  }

  for (int i = 0; i < nodes_.size(); i++) {
    auto node_a = nodes_[i].get();
    if (node_a->meta->type != OffloadedStmt::TaskType::listgen)
      continue;
    for (int j = i + 1; j < nodes_.size(); j++) {
      auto node_b = nodes_[j].get();
      if (node_b->meta->type != OffloadedStmt::TaskType::listgen)
        continue;
      if (node_a->meta->snode != node_b->meta->snode)
        continue;

      // Test if two list generations share the same mask and parent list
      auto snode = node_a->meta->snode;

      auto mask_state = AsyncState{snode, AsyncState::Type::mask};
      auto parent_list_state =
          AsyncState{snode->parent, AsyncState::Type::list};

      TI_ASSERT(node_a->input_edges[mask_state].size() == 1);
      TI_ASSERT(node_b->input_edges[mask_state].size() == 1);

      if (*node_a->input_edges[mask_state].begin() !=
          *node_b->input_edges[mask_state].begin())
        continue;

      TI_ASSERT(node_a->input_edges[parent_list_state].size() == 1);
      TI_ASSERT(node_b->input_edges[parent_list_state].size() == 1);
      if (*node_a->input_edges[parent_list_state].begin() !=
          *node_b->input_edges[parent_list_state].begin())
        continue;
      TI_INFO("Common list generation {} and {}", node_a->string(),
              node_b->string());
    }
  }

  return modified;
}

bool StateFlowGraph::fuse() {
  using bit::Bitset;
  const int n = nodes_.size();
  if (n <= 2) {
    return false;
  }

  reid_nodes();

  // Compute the transitive closure.
  auto has_path = std::make_unique<Bitset[]>(n);
  auto has_path_reverse = std::make_unique<Bitset[]>(n);
  // has_path[i][j] denotes if there is a path from i to j.
  // has_path_reverse[i][j] denotes if there is a path from j to i.
  for (int i = 0; i < n; i++) {
    has_path[i] = Bitset(n);
    has_path[i][i] = true;
    has_path_reverse[i] = Bitset(n);
    has_path_reverse[i][i] = true;
  }
  for (int i = n - 1; i >= 0; i--) {
    for (auto &edges : nodes_[i]->input_edges) {
      for (auto &edge : edges.second) {
        TI_ASSERT(edge->node_id < i);
        has_path[edge->node_id] |= has_path[i];
      }
    }
  }
  for (int i = 0; i < n; i++) {
    for (auto &edges : nodes_[i]->output_edges) {
      for (auto &edge : edges.second) {
        // Assume nodes are sorted in topological order.
        TI_ASSERT(edge->node_id > i);
        has_path_reverse[edge->node_id] |= has_path_reverse[i];
      }
    }
  }

  // Cache the result that if each pair is fusable by task types.
  // TODO: improve this
  auto task_type_fusable = std::make_unique<Bitset[]>(n);
  for (int i = 0; i < n; i++) {
    task_type_fusable[i] = Bitset(n);
  }
  // nodes_[0] is the initial node.
  for (int i = 1; i < n; i++) {
    auto &rec_i = nodes_[i]->rec;
    if (rec_i.empty()) {
      continue;
    }
    auto *task_i = rec_i.stmt();
    for (int j = i + 1; j < n; j++) {
      auto &rec_j = nodes_[j]->rec;
      if (rec_j.empty()) {
        continue;
      }
      auto *task_j = rec_j.stmt();
      bool is_same_struct_for =
          task_i->task_type == OffloadedStmt::struct_for &&
          task_j->task_type == OffloadedStmt::struct_for &&
          task_i->snode == task_j->snode &&
          task_i->block_dim == task_j->block_dim;
      // TODO: a few problems with the range-for test condition:
      // 1. This could incorrectly fuse two range-for kernels that have
      // different sizes, but then the loop ranges get padded to the same
      // power-of-two (E.g. maybe a side effect when a struct-for is demoted to
      // range-for).
      // 2. It has also fused range-fors that have the same linear range, but
      // are of different dimensions of loop indices, e.g. (16, ) and (4, 4).
      bool is_same_range_for = task_i->task_type == OffloadedStmt::range_for &&
                               task_j->task_type == OffloadedStmt::range_for &&
                               task_i->const_begin && task_j->const_begin &&
                               task_i->const_end && task_j->const_end &&
                               task_i->begin_value == task_j->begin_value &&
                               task_i->end_value == task_j->end_value;
      bool are_both_serial = task_i->task_type == OffloadedStmt::serial &&
                             task_j->task_type == OffloadedStmt::serial;
      const bool same_kernel = (rec_i.kernel == rec_j.kernel);
      bool kernel_args_match = true;
      if (!same_kernel) {
        // Merging kernels with different signatures will break invariants. E.g.
        // https://github.com/taichi-dev/taichi/blob/a6575fb97557267e2f550591f43b183076b72ac2/taichi/transforms/type_check.cpp#L326
        //
        // TODO: we could merge different kernels if their args are the same.
        // But we have no way to check that for now.
        auto check = [](const Kernel *k) {
          return (k->args.empty() && k->rets.empty());
        };
        kernel_args_match = (check(rec_i.kernel) && check(rec_j.kernel));
      }
      // TODO: avoid snode accessors going into async engine
      const bool is_snode_accessor =
          (rec_i.kernel->is_accessor || rec_j.kernel->is_accessor);
      bool fusable =
          (is_same_range_for || is_same_struct_for || are_both_serial) &&
          kernel_args_match && !is_snode_accessor;
      task_type_fusable[i][j] = fusable;
    }
  }

  auto insert_edge_for_transitive_closure = [&](int a, int b) {
    // insert edge a -> b
    auto update_list = has_path[a].or_eq_get_update_list(has_path[b]);
    for (auto i : update_list) {
      auto update_list_i =
          has_path_reverse[i].or_eq_get_update_list(has_path_reverse[a]);
      for (auto j : update_list_i) {
        has_path[i][j] = true;
      }
    }
  };

  auto do_fuse = [&](int a, int b) {
    auto *node_a = nodes_[a].get();
    auto *node_b = nodes_[b].get();
    // TODO: remove debug output
    TI_INFO("Fuse: {} <- {}", node_a->string(), node_b->string());
    auto &rec_a = node_a->rec;
    auto &rec_b = node_b->rec;
    // We are about to change both |task_a| and |task_b|. Clone them first.
    auto cloned_task_a = rec_a.ir_handle.clone();
    auto cloned_task_b = rec_b.ir_handle.clone();
    auto task_a = cloned_task_a->as<OffloadedStmt>();
    auto task_b = cloned_task_b->as<OffloadedStmt>();
    // TODO: in certain cases this optimization can be wrong!
    // Fuse task b into task_a
    for (int j = 0; j < (int)task_b->body->size(); j++) {
      task_a->body->insert(std::move(task_b->body->statements[j]));
    }
    task_b->body->statements.clear();

    // replace all reference to the offloaded statement B to A
    irpass::replace_all_usages_with(task_a, task_b, task_a);

    auto kernel = rec_a.kernel;
    irpass::full_simplify(task_a, /*after_lower_access=*/false, kernel);
    // For now, re_id is necessary for the hash to be correct.
    irpass::re_id(task_a);

    auto h = ir_bank_->get_hash(task_a);
    rec_a.ir_handle = IRHandle(task_a, h);
    ir_bank_->insert(std::move(cloned_task_a), h);
    rec_b.ir_handle = IRHandle(nullptr, 0);

    // TODO: since cloned_task_b->body is empty, can we remove this (i.e.,
    //  simply delete cloned_task_b here)?
    ir_bank_->insert_to_trash_bin(std::move(cloned_task_b));

    // replace all edges to the node B to A
    for (auto &edges : node_b->output_edges) {
      for (auto &edge : edges.second) {
        edge->input_edges[edges.first].erase(node_b);
        edge->input_edges[edges.first].insert(node_a);
      }
    }
    bool already_had_a_to_b_edge = false;
    for (auto &edges : node_b->input_edges) {
      for (auto &edge : edges.second) {
        edge->output_edges[edges.first].erase(node_b);
        if (edge == node_a)
          already_had_a_to_b_edge = true;
        else
          edge->output_edges[edges.first].insert(node_a);
      }
    }

    // update the transitive closure
    insert_edge_for_transitive_closure(b, a);
    if (!already_had_a_to_b_edge)
      insert_edge_for_transitive_closure(a, b);
  };

  auto fused = std::make_unique<bool[]>(n);

  bool modified = false;
  while (true) {
    bool updated = false;
    for (int i = 1; i < n; i++) {
      fused[i] = nodes_[i]->rec.empty();
    }
    for (int i = 1; i < n; i++) {
      if (!fused[i]) {
        bool i_updated = false;
        for (auto &edges : nodes_[i]->output_edges) {
          for (auto &edge : edges.second) {
            const int j = edge->node_id;
            // TODO: for each pair of edge (i, j), we can only fuse if they are
            //  both serial or both element-wise.
            if (!fused[j] && task_type_fusable[i][j]) {
              auto i_has_path_to_j = has_path[i] & has_path_reverse[j];
              i_has_path_to_j[i] = i_has_path_to_j[j] = false;
              // check if i doesn't have a path to j of length >= 2
              if (i_has_path_to_j.none()) {
                do_fuse(i, j);
                fused[i] = fused[j] = true;
                i_updated = true;
                updated = true;
                break;
              }
            }
          }
          if (i_updated)
            break;
        }
      }
    }
    // TODO: accelerate this
    for (int i = 1; i < n; i++) {
      if (!fused[i]) {
        for (int j = i + 1; j < n; j++) {
          if (!fused[j] && task_type_fusable[i][j] && !has_path[i][j] &&
              !has_path[j][i]) {
            do_fuse(i, j);
            fused[i] = fused[j] = true;
            updated = true;
            break;
          }
        }
      }
    }
    if (updated) {
      modified = true;
    } else {
      break;
    }
  }

  // Delete empty tasks. TODO: Do we need a trash bin here?
  if (modified) {
    std::vector<std::unique_ptr<Node>> new_nodes;
    new_nodes.reserve(n);
    new_nodes.push_back(std::move(nodes_[0]));
    for (int i = 1; i < n; i++) {
      if (!nodes_[i]->rec.empty()) {
        new_nodes.push_back(std::move(nodes_[i]));
      }
    }
    nodes_ = std::move(new_nodes);
  }

  // TODO: topo sorting after fusion crashes for some reason
  // topo_sort_nodes();

  return modified;
}

std::vector<TaskLaunchRecord> StateFlowGraph::extract() {
  std::vector<TaskLaunchRecord> tasks;
  tasks.reserve(nodes_.size());
  for (int i = 1; i < (int)nodes_.size(); i++) {
    tasks.push_back(nodes_[i]->rec);
  }
  clear();
  return tasks;
}

void StateFlowGraph::print() {
  fmt::print("=== State Flow Graph ===\n");
  for (auto &node : nodes_) {
    fmt::print("{}\n", node->string());
    if (!node->input_edges.empty()) {
      fmt::print("  Inputs:\n");
      for (const auto &p : node->input_edges) {
        for (const auto *to : p.second) {
          fmt::print("    {} <- {}\n", p.first.name(), to->string());
        }
      }
    }
    if (!node->output_edges.empty()) {
      fmt::print("  Outputs:\n");
      for (const auto &p : node->output_edges) {
        for (const auto *to : p.second) {
          fmt::print("    {} -> {}\n", p.first.name(), to->string());
        }
      }
    }
  }
  fmt::print("=======================\n");
}

std::string StateFlowGraph::dump_dot(
    const std::optional<std::string> &rankdir) {
  using SFGNode = StateFlowGraph::Node;
  using TaskType = OffloadedStmt::TaskType;
  std::stringstream ss;
  ss << "digraph {\n";
  auto node_id = [](const SFGNode *n) {
    // https://graphviz.org/doc/info/lang.html ID naming
    return fmt::format("n_{}_{}", n->meta->name, n->launch_id);
  };
  // Graph level configuration.
  if (rankdir) {
    ss << "  rankdir=" << *rankdir << "\n";
  }
  ss << "\n";
  // Specify the node styles
  std::unordered_set<const SFGNode *> latest_state_nodes;
  for (const auto &p : latest_state_owner_) {
    latest_state_nodes.insert(p.second);
  }
  std::vector<const SFGNode *> nodes_with_no_inputs;
  for (const auto &nd : nodes_) {
    const auto *n = nd.get();
    ss << "  " << fmt::format("{} [label=\"{}\"", node_id(n), n->string());
    if (nd->is_initial_node) {
      ss << ",shape=box";
    } else if (latest_state_nodes.find(n) != latest_state_nodes.end()) {
      ss << ",peripheries=2";
    }
    // Highlight user-defined tasks
    const auto tt = nd->meta->type;
    if (!nd->is_initial_node &&
        (tt == TaskType::range_for || tt == TaskType::struct_for ||
         tt == TaskType::serial)) {
      ss << ",style=filled,fillcolor=lightgray";
    }
    ss << "]\n";
    if (nd->input_edges.empty())
      nodes_with_no_inputs.push_back(n);
  }
  ss << "\n";
  {
    // DFS
    std::unordered_set<const SFGNode *> visited;
    std::vector<const SFGNode *> stack(nodes_with_no_inputs);
    while (!stack.empty()) {
      auto *from = stack.back();
      stack.pop_back();
      if (visited.find(from) == visited.end()) {
        visited.insert(from);
        for (const auto &p : from->output_edges) {
          for (const auto *to : p.second) {
            stack.push_back(to);
            std::string style;

            if (!from->has_state_flow(p.first, to)) {
              style = "style=dotted";
            }

            ss << "  "
               << fmt::format("{} -> {} [label=\"{}\" {}]", node_id(from),
                              node_id(to), p.first.name(), style)
               << '\n';
          }
        }
      }
    }
  }
  ss << "}\n";  // closes "dirgraph {"
  return ss.str();
}

void StateFlowGraph::topo_sort_nodes() {
  std::deque<std::unique_ptr<Node>> queue;
  std::vector<std::unique_ptr<Node>> new_nodes;
  std::vector<int> degrees_in(nodes_.size());

  reid_nodes();

  for (auto &node : nodes_) {
    int degree_in = 0;
    for (auto &inputs : node->input_edges) {
      degree_in += (int)inputs.second.size();
    }
    degrees_in[node->node_id] = degree_in;
  }

  queue.emplace_back(std::move(nodes_[0]));

  while (!queue.empty()) {
    TI_P(queue.size());
    auto head = std::move(queue.front());
    queue.pop_front();

    // Delete the node and update degrees_in
    for (auto &output_edge : head->output_edges) {
      for (auto &e : output_edge.second) {
        auto dest = e->node_id;
        degrees_in[dest]--;
        TI_P(degrees_in[dest]);
        TI_ASSERT(degrees_in[dest] >= 0);
        if (degrees_in[dest] == 0) {
          queue.push_back(std::move(nodes_[dest]));
        }
      }
    }

    new_nodes.emplace_back(std::move(head));
  }

  TI_ASSERT(new_nodes.size() == nodes_.size());
  nodes_ = std::move(new_nodes);
  reid_nodes();
}

void StateFlowGraph::reid_nodes() {
  for (int i = 0; i < nodes_.size(); i++) {
    nodes_[i]->node_id = i;
  }
  TI_ASSERT(initial_node_->node_id == 0);
}

void async_print_sfg() {
  get_current_program().async_engine->sfg->print();
}

std::string async_dump_dot(std::optional<std::string> rankdir) {
  // https://pybind11.readthedocs.io/en/stable/advanced/functions.html#allow-prohibiting-none-arguments
  return get_current_program().async_engine->sfg->dump_dot(rankdir);
}

TLANG_NAMESPACE_END
