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

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ninja/build_log.hpp>
#include <ninja/graph.hpp>
#include <ninja/manifest_parser.hpp>
#include <ninja/test.hpp>
#include <ninja/util.hpp>
#include <unistd.h>

#ifdef _AIX
extern "C" {
// GCC "helpfully" strips the definition of mkdtemp out on AIX.
// The function is still present, so if we define it ourselves
// it will work perfectly fine.
extern char*
mkdtemp(char* name_template);
}
#endif

namespace {

std::string
GetSystemTempDir() {
  const char* tempdir = getenv("TMPDIR");
  if (tempdir)
    return tempdir;
  return "/tmp";
}

} // anonymous namespace

StateTestWithBuiltinRules::StateTestWithBuiltinRules() { AddCatRule(&state_); }

void
StateTestWithBuiltinRules::AddCatRule(State* state) {
  AssertParse(
      state,
      "rule cat\n"
      "  command = cat $in > $out\n"
  );
}

Node*
StateTestWithBuiltinRules::GetNode(const std::string& path) {
  EXPECT_FALSE(strpbrk(path.c_str(), "/\\"));
  return state_.GetNode(path, 0);
}

void
AssertParse(State* state, const char* input, ManifestParserOptions opts) {
  ManifestParser parser(state, nullptr, opts);
  std::string err;
  EXPECT_TRUE(parser.ParseTest(input, &err));
  ASSERT_EQ("", err);
  VerifyGraph(*state);
}

void
AssertHash(const char* expected, uint64_t actual) {
  ASSERT_EQ(BuildLog::LogEntry::HashCommand(expected), actual);
}

void
VerifyGraph(const State& state) {
  for (const std::unique_ptr<Edge>& edge : state.edges_) {
    // All edges need at least one output.
    EXPECT_FALSE(edge->outputs_.empty());
    // Check that the edge's inputs have the edge as out-edge.
    for (Node* in_node : edge->inputs_) {
      const std::vector<Edge*>& out_edges = in_node->out_edges();
      EXPECT_NE(find(out_edges.begin(), out_edges.end(), edge.get()), out_edges.end());
    }
    // Check that the edge's outputs have the edge as in-edge.
    for (Node* out_node : edge->outputs_) {
      EXPECT_EQ(out_node->in_edge(), edge.get());
    }
  }

  // The union of all in- and out-edges of each nodes should be exactly edges_.
  std::set<const Edge*> node_edge_set;
  for (const auto& path : state.paths_) {
    const Node* n = path.second.get();
    if (n->in_edge())
      node_edge_set.insert(n->in_edge());
    node_edge_set.insert(n->out_edges().begin(), n->out_edges().end());
  }
  std::set<const Edge*> edge_set;
  for (const std::unique_ptr<Edge>& e : state.edges_) {
    edge_set.insert(e.get());
  }
  EXPECT_EQ(node_edge_set, edge_set);
}

void
VirtualFileSystem::Create(
    const std::string& path, const std::string& contents
) {
  files_[path].mtime = now_;
  files_[path].contents = contents;
  files_created_.insert(path);
}

TimeStamp
VirtualFileSystem::Stat(const std::string& path, std::string* err) const {
  FileMap::const_iterator i = files_.find(path);
  if (i != files_.end()) {
    *err = i->second.stat_error;
    return i->second.mtime;
  }
  return 0;
}

bool
VirtualFileSystem::WriteFile(
    const std::string& path, const std::string& contents
) {
  Create(path, contents);
  return true;
}

bool
VirtualFileSystem::MakeDir(const std::string& path) {
  directories_made_.push_back(path);
  return true; // success
}

FileReader::Status
VirtualFileSystem::ReadFile(
    const std::string& path, std::string* contents, std::string* err
) {
  files_read_.push_back(path);
  FileMap::iterator i = files_.find(path);
  if (i != files_.end()) {
    *contents = i->second.contents;
    return Okay;
  }
  *err = strerror(ENOENT);
  return NotFound;
}

int
VirtualFileSystem::RemoveFile(const std::string& path) {
  if (find(directories_made_.begin(), directories_made_.end(), path)
      != directories_made_.end())
    return -1;
  FileMap::iterator i = files_.find(path);
  if (i != files_.end()) {
    files_.erase(i);
    files_removed_.insert(path);
    return 0;
  } else {
    return 1;
  }
}

void
ScopedTempDir::CreateAndEnter(const std::string& name) {
  // First change into the system temp dir and save it for cleanup.
  start_dir_ = GetSystemTempDir();
  if (start_dir_.empty())
    Fatal("couldn't get system temp dir");
  if (chdir(start_dir_.c_str()) < 0)
    Fatal("chdir: %s", strerror(errno));

  // Create a temporary subdirectory of that.
  char name_template[1024];
  strcpy(name_template, name.c_str());
  strcat(name_template, "-XXXXXX");
  char* tempname = mkdtemp(name_template);
  if (!tempname)
    Fatal("mkdtemp: %s", strerror(errno));
  temp_dir_name_ = tempname;

  // chdir into the new temporary directory.
  if (chdir(temp_dir_name_.c_str()) < 0)
    Fatal("chdir: %s", strerror(errno));
}

void
ScopedTempDir::Cleanup() {
  if (temp_dir_name_.empty())
    return; // Something went wrong earlier.

  // Move out of the directory we're about to clobber.
  if (chdir(start_dir_.c_str()) < 0)
    Fatal("chdir: %s", strerror(errno));

  std::string command = "rm -rf " + temp_dir_name_;
  if (system(command.c_str()) < 0)
    Fatal("system: %s", strerror(errno));

  temp_dir_name_.clear();
}
