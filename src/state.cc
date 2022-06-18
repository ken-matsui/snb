// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cassert>
#include <cstdio>
#include <ninja/edit_distance.hpp>
#include <ninja/graph.hpp>
#include <ninja/state.hpp>
#include <ninja/util.hpp>

void
Pool::EdgeScheduled(const Edge& edge) {
  if (depth_ != 0)
    current_use_ += edge.weight();
}

void
Pool::EdgeFinished(const Edge& edge) {
  if (depth_ != 0)
    current_use_ -= edge.weight();
}

void
Pool::DelayEdge(Edge* edge) {
  assert(depth_ != 0);
  delayed_.insert(edge);
}

void
Pool::RetrieveReadyEdges(EdgeSet* ready_queue) {
  DelayedEdges::iterator it = delayed_.begin();
  while (it != delayed_.end()) {
    Edge* edge = *it;
    if (current_use_ + edge->weight() > depth_)
      break;
    ready_queue->insert(edge);
    EdgeScheduled(*edge);
    ++it;
  }
  delayed_.erase(delayed_.begin(), it);
}

void
Pool::Dump() const {
  printf("%s (%d/%d) ->\n", name_.c_str(), current_use_, depth_);
  for (Edge* it : delayed_) {
    printf("\t");
    it->Dump();
  }
}

Pool State::kDefaultPool("", 0);
Pool State::kConsolePool("console", 1);
const Rule State::kPhonyRule("phony");

State::State() {
  bindings_.AddRule(&kPhonyRule);
  AddPool(&kDefaultPool);
  AddPool(&kConsolePool);
}

void
State::AddPool(Pool* pool) {
  const std::string& pool_name = pool->name();
  assert(LookupPool(pool_name) == nullptr);
  pools_[pool_name] = pool;
}

Pool*
State::LookupPool(const std::string& pool_name) {
  const auto i = pools_.find(pool_name);
  if (i == pools_.end())
    return nullptr;
  return i->second;
}

Edge*
State::AddEdge(const Rule* rule) {
  std::unique_ptr<Edge> edge = std::make_unique<Edge>();
  edge->rule_ = rule;
  edge->pool_ = &State::kDefaultPool;
  edge->env_ = &bindings_;
  edge->id_ = edges_.size();
  edges_.push_back(std::move(edge));
  return edges_.back().get();
}

Node*
State::GetNode(std::string_view path, uint64_t slash_bits) {
  Node* node = LookupNode(path);
  if (node)
    return node;
  std::unique_ptr<Node> node_ptr = std::make_unique<Node>(std::string(path), slash_bits);
  node = node_ptr.get();
  paths_[node->path()] = std::move(node_ptr);
  return node;
}

Node*
State::LookupNode(std::string_view path) const {
  Paths::const_iterator i = paths_.find(path);
  if (i != paths_.end())
    return i->second.get();
  return nullptr;
}

Node*
State::SpellcheckNode(const std::string& path) {
  const bool kAllowReplacements = true;
  const int kMaxValidEditDistance = 3;

  int min_distance = kMaxValidEditDistance + 1;
  Node* result = nullptr;
  for (const auto& i : paths_) {
    int distance =
        EditDistance(i.first, path, kAllowReplacements, kMaxValidEditDistance);
    if (distance < min_distance && i.second) {
      min_distance = distance;
      result = i.second.get();
    }
  }
  return result;
}

void
State::AddIn(Edge* edge, std::string_view path, uint64_t slash_bits) {
  Node* node = GetNode(path, slash_bits);
  edge->inputs_.push_back(node);
  node->AddOutEdge(edge);
}

bool
State::AddOut(Edge* edge, std::string_view path, uint64_t slash_bits) {
  Node* node = GetNode(path, slash_bits);
  if (node->in_edge())
    return false;
  edge->outputs_.push_back(node);
  node->set_in_edge(edge);
  return true;
}

void
State::AddValidation(Edge* edge, std::string_view path, uint64_t slash_bits) {
  Node* node = GetNode(path, slash_bits);
  edge->validations_.push_back(node);
  node->AddValidationOutEdge(edge);
}

bool
State::AddDefault(std::string_view path, std::string* err) {
  Node* node = LookupNode(path);
  if (!node) {
    *err = "unknown target '" + std::string(path) + "'";
    return false;
  }
  defaults_.push_back(node);
  return true;
}

std::vector<Node*>
State::RootNodes(std::string* err) const {
  std::vector<Node*> root_nodes;
  // Search for nodes with no output.
  for (const std::unique_ptr<Edge>& edge : edges_) {
    for (Node* output : edge->outputs_) {
      if (output->out_edges().empty())
        root_nodes.push_back(output);
    }
  }

  if (!edges_.empty() && root_nodes.empty())
    *err = "could not determine root nodes of build graph";

  return root_nodes;
}

std::vector<Node*>
State::DefaultNodes(std::string* err) const {
  return defaults_.empty() ? RootNodes(err) : defaults_;
}

void
State::Reset() {
  for (const auto& path : paths_)
    path.second->ResetState();
  for (std::unique_ptr<Edge>& edge : edges_) {
    edge->outputs_ready_ = false;
    edge->deps_loaded_ = false;
    edge->mark_ = Edge::VisitNone;
  }
}

void
State::Dump() {
  for (Paths::iterator i = paths_.begin(); i != paths_.end(); ++i) {
    Node* node = i->second.get();
    printf(
        "%s %s [id:%d]\n", node->path().c_str(),
        node->status_known() ? (node->dirty() ? "dirty" : "clean") : "unknown",
        node->id()
    );
  }
  if (!pools_.empty()) {
    printf("resource_pools:\n");
    for (const auto& pool : pools_) {
      if (!pool.second->name().empty()) {
        pool.second->Dump();
      }
    }
  }
}
