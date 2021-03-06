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
#include <ninja/clean.hpp>
#include <ninja/disk_interface.hpp>
#include <ninja/graph.hpp>
#include <ninja/state.hpp>
#include <ninja/util.hpp>

Cleaner::Cleaner(
    State* state, const BuildConfig& config, DiskInterface* disk_interface
)
    : state_(state), config_(config), dyndep_loader_(state, disk_interface),
      cleaned_files_count_(0), disk_interface_(disk_interface), status_(0) {}

int
Cleaner::RemoveFile(const std::string& path) {
  return disk_interface_->RemoveFile(path);
}

bool
Cleaner::FileExists(const std::string& path) {
  std::string err;
  TimeStamp mtime = disk_interface_->Stat(path, &err);
  if (mtime == -1)
    Error("%s", err.c_str());
  return mtime > 0; // Treat Stat() errors as "file does not exist".
}

void
Cleaner::Report(const std::string& path) {
  ++cleaned_files_count_;
  if (IsVerbose())
    printf("Remove %s\n", path.c_str());
}

void
Cleaner::Remove(const std::string& path) {
  if (!IsAlreadyRemoved(path)) {
    removed_.insert(path);
    if (config_.dry_run) {
      if (FileExists(path))
        Report(path);
    } else {
      int ret = RemoveFile(path);
      if (ret == 0)
        Report(path);
      else if (ret == -1)
        status_ = 1;
    }
  }
}

bool
Cleaner::IsAlreadyRemoved(const std::string& path) {
  std::set<std::string>::iterator i = removed_.find(path);
  return (i != removed_.end());
}

void
Cleaner::RemoveEdgeFiles(Edge* edge) {
  std::string depfile = edge->GetUnescapedDepfile();
  if (!depfile.empty())
    Remove(depfile);

  std::string rspfile = edge->GetUnescapedRspfile();
  if (!rspfile.empty())
    Remove(rspfile);
}

void
Cleaner::PrintHeader() {
  if (config_.verbosity == BuildConfig::QUIET)
    return;
  printf("Cleaning...");
  if (IsVerbose())
    printf("\n");
  else
    printf(" ");
  fflush(stdout);
}

void
Cleaner::PrintFooter() {
  if (config_.verbosity == BuildConfig::QUIET)
    return;
  printf("%d files.\n", cleaned_files_count_);
}

int
Cleaner::CleanAll(bool generator) {
  Reset();
  PrintHeader();
  LoadDyndeps();
  for (std::unique_ptr<Edge>& edge : state_->edges_) {
    // Do not try to remove phony targets
    if (edge->is_phony())
      continue;
    // Do not remove generator's files unless generator specified.
    if (!generator && edge->GetBindingBool("generator"))
      continue;
    for (Node* output : edge->outputs_) {
      Remove(output->path());
    }

    RemoveEdgeFiles(edge.get());
  }
  PrintFooter();
  return status_;
}

int
Cleaner::CleanDead(const BuildLog::Entries& entries) {
  Reset();
  PrintHeader();
  for (BuildLog::Entries::const_iterator i = entries.begin();
       i != entries.end(); ++i) {
    Node* n = state_->LookupNode(i->first);
    // Detecting stale outputs works as follows:
    //
    // - If it has no Node, it is not in the build graph, or the deps log
    //   anymore, hence is stale.
    //
    // - If it isn't an output or input for any edge, it comes from a stale
    //   entry in the deps log, but no longer referenced from the build
    //   graph.
    //
    if (!n || (!n->in_edge() && n->out_edges().empty())) {
      Remove(std::string(i->first));
    }
  }
  PrintFooter();
  return status_;
}

void
Cleaner::DoCleanTarget(Node* target) {
  if (Edge* e = target->in_edge()) {
    // Do not try to remove phony targets
    if (!e->is_phony()) {
      Remove(target->path());
      RemoveEdgeFiles(e);
    }
    for (std::vector<Node*>::iterator n = e->inputs_.begin();
         n != e->inputs_.end(); ++n) {
      Node* next = *n;
      // call DoCleanTarget recursively if this node has not been visited
      if (cleaned_.count(next) == 0) {
        DoCleanTarget(next);
      }
    }
  }

  // mark this target to be cleaned already
  cleaned_.insert(target);
}

int
Cleaner::CleanTarget(Node* target) {
  assert(target);

  Reset();
  PrintHeader();
  LoadDyndeps();
  DoCleanTarget(target);
  PrintFooter();
  return status_;
}

int
Cleaner::CleanTarget(const char* target) {
  assert(target);

  Reset();
  Node* node = state_->LookupNode(target);
  if (node) {
    CleanTarget(node);
  } else {
    Error("unknown target '%s'", target);
    status_ = 1;
  }
  return status_;
}

int
Cleaner::CleanTargets(int target_count, char* targets[]) {
  Reset();
  PrintHeader();
  LoadDyndeps();
  for (int i = 0; i < target_count; ++i) {
    std::string target_name = targets[i];
    if (target_name.empty()) {
      Error("failed to canonicalize '': empty path");
      status_ = 1;
      continue;
    }
    uint64_t slash_bits;
    CanonicalizePath(&target_name, &slash_bits);
    Node* target = state_->LookupNode(target_name);
    if (target) {
      if (IsVerbose())
        printf("Target %s\n", target_name.c_str());
      DoCleanTarget(target);
    } else {
      Error("unknown target '%s'", target_name.c_str());
      status_ = 1;
    }
  }
  PrintFooter();
  return status_;
}

void
Cleaner::DoCleanRule(const Rule* rule) {
  assert(rule);

  for (std::unique_ptr<Edge>& edge : state_->edges_) {
    if (edge->rule().name() == rule->name()) {
      for (std::vector<Node*>::iterator out_node = edge->outputs_.begin();
           out_node != edge->outputs_.end(); ++out_node) {
        Remove((*out_node)->path());
        RemoveEdgeFiles(edge.get());
      }
    }
  }
}

int
Cleaner::CleanRule(const Rule* rule) {
  assert(rule);

  Reset();
  PrintHeader();
  LoadDyndeps();
  DoCleanRule(rule);
  PrintFooter();
  return status_;
}

int
Cleaner::CleanRule(const char* rule) {
  assert(rule);

  Reset();
  const Rule* r = state_->bindings_.LookupRule(rule);
  if (r) {
    CleanRule(r);
  } else {
    Error("unknown rule '%s'", rule);
    status_ = 1;
  }
  return status_;
}

int
Cleaner::CleanRules(int rule_count, char* rules[]) {
  assert(rules);

  Reset();
  PrintHeader();
  LoadDyndeps();
  for (int i = 0; i < rule_count; ++i) {
    const char* rule_name = rules[i];
    const Rule* rule = state_->bindings_.LookupRule(rule_name);
    if (rule) {
      if (IsVerbose())
        printf("Rule %s\n", rule_name);
      DoCleanRule(rule);
    } else {
      Error("unknown rule '%s'", rule_name);
      status_ = 1;
    }
  }
  PrintFooter();
  return status_;
}

void
Cleaner::Reset() {
  status_ = 0;
  cleaned_files_count_ = 0;
  removed_.clear();
  cleaned_.clear();
}

void
Cleaner::LoadDyndeps() {
  // Load dyndep files that exist, before they are cleaned.
  for (std::unique_ptr<Edge>& edge : state_->edges_) {
    if (Node* dyndep = edge->dyndep_) {
      // Capture and ignore errors loading the dyndep file.
      // We clean as much of the graph as we know.
      std::string err;
      dyndep_loader_.LoadDyndeps(dyndep, &err);
    }
  }
}
