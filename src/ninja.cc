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
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _AIX
#  include "getopt.h"

#  include <unistd.h>
#else
#  include <getopt.h>
#  include <unistd.h>
#endif

#include <ninja/browse.hpp>
#include <ninja/build.hpp>
#include <ninja/build_log.hpp>
#include <ninja/clean.hpp>
#include <ninja/debug_flags.hpp>
#include <ninja/depfile_parser.hpp>
#include <ninja/deps_log.hpp>
#include <ninja/disk_interface.hpp>
#include <ninja/graph.hpp>
#include <ninja/graphviz.hpp>
#include <ninja/json.hpp>
#include <ninja/manifest_parser.hpp>
#include <ninja/metrics.hpp>
#include <ninja/missing_deps.hpp>
#include <ninja/state.hpp>
#include <ninja/status.hpp>
#include <ninja/util.hpp>
#include <ninja/version.hpp>

namespace {

struct Tool;

/// Command-line options.
struct Options {
  /// Build file to load.
  const char* input_file;

  /// Directory to change into before running.
  const char* working_dir;

  /// Tool to run rather than building.
  const Tool* tool;

  /// Whether duplicate rules for one target should warn or print an error.
  bool dupe_edges_should_err;

  /// Whether phony cycles should warn or print an error.
  bool phony_cycle_should_err;
};

/// The Ninja main() loads up a series of data structures; various tools need
/// to poke into these, so store them as fields on an object.
struct NinjaMain : public BuildLogUser {
  NinjaMain(const char* ninja_command, const BuildConfig& config)
      : ninja_command_(ninja_command), config_(config),
        start_time_millis_(GetTimeMillis()) {}

  /// Command line used to run Ninja.
  const char* ninja_command_;

  /// Build configuration set from flags (e.g. parallelism).
  const BuildConfig& config_;

  /// Loaded state (rules, nodes).
  State state_;

  /// Functions for accessing the disk.
  RealDiskInterface disk_interface_;

  /// The build directory, used for storing the build log etc.
  std::string build_dir_;

  BuildLog build_log_;
  DepsLog deps_log_;

  /// The type of functions that are the entry points to tools (subcommands).
  typedef int (NinjaMain::*ToolFunc)(const Options*, int, char**);

  /// Get the Node for a given command-line path, handling features like
  /// spell correction.
  Node*
  CollectTarget(const char* cpath, std::string* err);

  /// CollectTarget for all command-line arguments, filling in \a targets.
  bool
  CollectTargetsFromArgs(
      int argc, char* argv[], std::vector<Node*>* targets, std::string* err
  );

  // The various subcommands, run via "-t XXX".
  int
  ToolGraph(const Options* options, int argc, char* argv[]);
  int
  ToolQuery(const Options* options, int argc, char* argv[]);
  int
  ToolDeps(const Options* options, int argc, char* argv[]);
  int
  ToolMissingDeps(const Options* options, int argc, char* argv[]);
  int
  ToolBrowse(const Options* options, int argc, char* argv[]);
  int
  ToolMSVC(const Options* options, int argc, char* argv[]);
  int
  ToolTargets(const Options* options, int argc, char* argv[]);
  int
  ToolCommands(const Options* options, int argc, char* argv[]);
  int
  ToolInputs(const Options* options, int argc, char* argv[]);
  int
  ToolClean(const Options* options, int argc, char* argv[]);
  int
  ToolCleanDead(const Options* options, int argc, char* argv[]);
  int
  ToolCompilationDatabase(const Options* options, int argc, char* argv[]);
  int
  ToolRecompact(const Options* options, int argc, char* argv[]);
  int
  ToolRestat(const Options* options, int argc, char* argv[]);
  int
  ToolUrtle(const Options* options, int argc, char** argv);
  int
  ToolRules(const Options* options, int argc, char* argv[]);
  int
  ToolWinCodePage(const Options* options, int argc, char* argv[]);

  /// Open the build log.
  /// @return false on error.
  bool
  OpenBuildLog(bool recompact_only = false);

  /// Open the deps log: load it, then open for writing.
  /// @return false on error.
  bool
  OpenDepsLog(bool recompact_only = false);

  /// Ensure the build directory exists, creating it if necessary.
  /// @return false on error.
  bool
  EnsureBuildDirExists();

  /// Rebuild the manifest, if necessary.
  /// Fills in \a err on error.
  /// @return true if the manifest was rebuilt.
  bool
  RebuildManifest(const char* input_file, std::string* err, Status* status);

  /// Build the targets listed on the command line.
  /// @return an exit code.
  int
  RunBuild(int argc, char** argv, Status* status);

  /// Dump the output requested by '-d stats'.
  void
  DumpMetrics();

  virtual bool
  IsPathDead(std::string_view s) const {
    Node* n = state_.LookupNode(s);
    if (n && n->in_edge())
      return false;
    // Just checking n isn't enough: If an old output is both in the build log
    // and in the deps log, it will have a Node object in state_.  (It will also
    // have an in edge if one of its inputs is another output that's in the deps
    // log, but having a deps edge product an output that's input to another
    // deps edge is rare, and the first recompaction will delete all old outputs
    // from the deps log, and then a second recompaction will clear the build
    // log, which seems good enough for this corner case.) Do keep entries
    // around for files which still exist on disk, for generators that want to
    // use this information.
    std::string err;
    TimeStamp mtime = disk_interface_.Stat(std::string(s), &err);
    if (mtime == -1)
      Error("%s", err.c_str()); // Log and ignore Stat() errors.
    return mtime == 0;
  }

  int64_t start_time_millis_;
};

/// Subtools, accessible via "-t foo".
struct Tool {
  /// Short name of the tool.
  const char* name;

  /// Description (shown in "-t list").
  const char* desc;

  /// When to run the tool.
  enum {
    /// Run after parsing the command-line flags and potentially changing
    /// the current working directory (as early as possible).
    RUN_AFTER_FLAGS,

    /// Run after loading build.ninja.
    RUN_AFTER_LOAD,

    /// Run after loading the build/deps logs.
    RUN_AFTER_LOGS,
  } when;

  /// Implementation of the tool.
  NinjaMain::ToolFunc func;
};

/// Print usage information.
void
Usage(const BuildConfig& config) {
  fprintf(
      stderr,
      "usage: ninja [options] [targets...]\n"
      "\n"
      "if targets are unspecified, builds the 'default' target (see manual).\n"
      "\n"
      "options:\n"
      "  --version      print ninja version (\"%s\")\n"
      "  -v, --verbose  show all command lines while building\n"
      "  --quiet        don't show progress status, just command output\n"
      "\n"
      "  -C DIR   change to DIR before doing anything else\n"
      "  -f FILE  specify input build file [default=build.ninja]\n"
      "\n"
      "  -j N     run N jobs in parallel (0 means infinity) [default=%d on this system]\n"
      "  -k N     keep going until N jobs fail (0 means infinity) [default=1]\n"
      "  -l N     do not start new jobs if the load average is greater than N\n"
      "  -n       dry run (don't run commands but act like they succeeded)\n"
      "\n"
      "  -d MODE  enable debugging (use '-d list' to list modes)\n"
      "  -t TOOL  run a subtool (use '-t list' to list subtools)\n"
      "    terminates toplevel options; further flags are passed to the tool\n"
      "  -w FLAG  adjust warnings (use '-w list' to list warnings)\n",
      kNinjaVersion, config.parallelism
  );
}

/// Choose a default value for the -j (parallelism) flag.
int
GuessParallelism() {
  switch (int processors = GetProcessorCount()) {
    case 0:
    case 1:
      return 2;
    case 2:
      return 3;
    default:
      return processors + 2;
  }
}

/// Rebuild the build manifest, if necessary.
/// Returns true if the manifest was rebuilt.
bool
NinjaMain::RebuildManifest(
    const char* input_file, std::string* err, Status* status
) {
  std::string path = input_file;
  if (path.empty()) {
    *err = "empty path";
    return false;
  }
  uint64_t slash_bits; // Unused because this path is only used for lookup.
  CanonicalizePath(&path, &slash_bits);
  Node* node = state_.LookupNode(path);
  if (!node)
    return false;

  Builder builder(
      &state_, config_, &build_log_, &deps_log_, &disk_interface_, status,
      start_time_millis_
  );
  if (!builder.AddTarget(node, err))
    return false;

  if (builder.AlreadyUpToDate())
    return false; // Not an error, but we didn't rebuild.

  if (!builder.Build(err))
    return false;

  // The manifest was only rebuilt if it is now dirty (it may have been cleaned
  // by a restat).
  if (!node->dirty()) {
    // Reset the state to prevent problems like
    // https://github.com/ninja-build/ninja/issues/874
    state_.Reset();
    return false;
  }

  return true;
}

Node*
NinjaMain::CollectTarget(const char* cpath, std::string* err) {
  std::string path = cpath;
  if (path.empty()) {
    *err = "empty path";
    return nullptr;
  }
  uint64_t slash_bits;
  CanonicalizePath(&path, &slash_bits);

  // Special syntax: "foo.cc^" means "the first output of foo.cc".
  bool first_dependent = false;
  if (!path.empty() && path[path.size() - 1] == '^') {
    path.resize(path.size() - 1);
    first_dependent = true;
  }

  Node* node = state_.LookupNode(path);
  if (node) {
    if (first_dependent) {
      if (node->out_edges().empty()) {
        Node* rev_deps = deps_log_.GetFirstReverseDepsNode(node);
        if (!rev_deps) {
          *err = "'" + path + "' has no out edge";
          return nullptr;
        }
        node = rev_deps;
      } else {
        Edge* edge = node->out_edges()[0];
        if (edge->outputs_.empty()) {
          edge->Dump();
          Fatal("edge has no outputs");
        }
        node = edge->outputs_[0];
      }
    }
    return node;
  } else {
    *err =
        "unknown target '" + Node::PathDecanonicalized(path, slash_bits) + "'";
    if (path == "clean") {
      *err += ", did you mean 'ninja -t clean'?";
    } else if (path == "help") {
      *err += ", did you mean 'ninja -h'?";
    } else {
      Node* suggestion = state_.SpellcheckNode(path);
      if (suggestion) {
        *err += ", did you mean '" + suggestion->path() + "'?";
      }
    }
    return nullptr;
  }
}

bool
NinjaMain::CollectTargetsFromArgs(
    int argc, char* argv[], std::vector<Node*>* targets, std::string* err
) {
  if (argc == 0) {
    *targets = state_.DefaultNodes(err);
    return err->empty();
  }

  for (int i = 0; i < argc; ++i) {
    Node* node = CollectTarget(argv[i], err);
    if (node == nullptr)
      return false;
    targets->push_back(node);
  }
  return true;
}

int
NinjaMain::ToolGraph(const Options* options, int argc, char* argv[]) {
  std::vector<Node*> nodes;
  std::string err;
  if (!CollectTargetsFromArgs(argc, argv, &nodes, &err)) {
    Error("%s", err.c_str());
    return 1;
  }

  GraphViz graph(&state_, &disk_interface_);
  graph.Start();
  for (std::vector<Node*>::const_iterator n = nodes.begin(); n != nodes.end();
       ++n)
    graph.AddTarget(*n);
  graph.Finish();

  return 0;
}

int
NinjaMain::ToolQuery(const Options* options, int argc, char* argv[]) {
  if (argc == 0) {
    Error("expected a target to query");
    return 1;
  }

  DyndepLoader dyndep_loader(&state_, &disk_interface_);

  for (int i = 0; i < argc; ++i) {
    std::string err;
    Node* node = CollectTarget(argv[i], &err);
    if (!node) {
      Error("%s", err.c_str());
      return 1;
    }

    printf("%s:\n", node->path().c_str());
    if (Edge* edge = node->in_edge()) {
      if (edge->dyndep_ && edge->dyndep_->dyndep_pending()) {
        if (!dyndep_loader.LoadDyndeps(edge->dyndep_, &err)) {
          Warning("%s\n", err.c_str());
        }
      }
      printf("  input: %s\n", edge->rule_->name().c_str());
      for (int in = 0; in < (int)edge->inputs_.size(); in++) {
        const char* label = "";
        if (edge->is_implicit(in))
          label = "| ";
        else if (edge->is_order_only(in))
          label = "|| ";
        printf("    %s%s\n", label, edge->inputs_[in]->path().c_str());
      }
      if (!edge->validations_.empty()) {
        printf("  validations:\n");
        for (Node* validation : edge->validations_) {
          printf("    %s\n", validation->path().c_str());
        }
      }
    }
    printf("  outputs:\n");
    for (Edge* edge : node->out_edges()) {
      for (Node* output : edge->outputs_) {
        printf("    %s\n", output->path().c_str());
      }
    }
    const std::vector<Edge*> validation_edges = node->validation_out_edges();
    if (!validation_edges.empty()) {
      printf("  validation for:\n");
      for (Edge* validation_edge : validation_edges) {
        for (Node* output : validation_edge->outputs_) {
          printf("    %s\n", output->path().c_str());
        }
      }
    }
  }
  return 0;
}

#if defined(NINJA_HAVE_BROWSE)
int
NinjaMain::ToolBrowse(const Options* options, int argc, char* argv[]) {
  RunBrowsePython(&state_, ninja_command_, options->input_file, argc, argv);
  // If we get here, the browse failed.
  return 1;
}
#else
int
NinjaMain::ToolBrowse(const Options*, int, char**) {
  Fatal("browse tool not supported on this platform");
  return 1;
}
#endif

#if defined(_WIN32)
int
NinjaMain::ToolMSVC(const Options* options, int argc, char* argv[]) {
  // Reset getopt: push one argument onto the front of argv, reset optind.
  argc++;
  argv--;
  optind = 0;
  return MSVCHelperMain(argc, argv);
}
#endif

int
ToolTargetsList(const std::vector<Node*>& nodes, int depth, int indent) {
  for (std::vector<Node*>::const_iterator n = nodes.begin(); n != nodes.end();
       ++n) {
    for (int i = 0; i < indent; ++i)
      printf("  ");
    const char* target = (*n)->path().c_str();
    if ((*n)->in_edge()) {
      printf("%s: %s\n", target, (*n)->in_edge()->rule_->name().c_str());
      if (depth > 1 || depth <= 0)
        ToolTargetsList((*n)->in_edge()->inputs_, depth - 1, indent + 1);
    } else {
      printf("%s\n", target);
    }
  }
  return 0;
}

int
ToolTargetsSourceList(State* state) {
  for (std::unique_ptr<Edge>& edge : state->edges_) {
    for (Node* input : edge->inputs_) {
      if (!input->in_edge())
        printf("%s\n", input->path().c_str());
    }
  }
  return 0;
}

int
ToolTargetsList(State* state, const std::string& rule_name) {
  std::set<std::string> rules;

  // Gather the outputs.
  for (std::unique_ptr<Edge>& edge : state->edges_) {
    if (edge->rule_->name() == rule_name) {
      for (Node* output : edge->outputs_) {
        rules.insert(output->path());
      }
    }
  }

  // Print them.
  for (const std::string& rule : rules) {
    printf("%s\n", rule.c_str());
  }

  return 0;
}

int
ToolTargetsList(State* state) {
  for (std::unique_ptr<Edge>& edge : state->edges_) {
    for (std::vector<Node*>::iterator out_node = edge->outputs_.begin();
         out_node != edge->outputs_.end(); ++out_node) {
      printf(
          "%s: %s\n", (*out_node)->path().c_str(), edge->rule_->name().c_str()
      );
    }
  }
  return 0;
}

int
NinjaMain::ToolDeps(const Options* options, int argc, char** argv) {
  std::vector<Node*> nodes;
  if (argc == 0) {
    for (std::vector<Node*>::const_iterator ni = deps_log_.nodes().begin();
         ni != deps_log_.nodes().end(); ++ni) {
      if (deps_log_.IsDepsEntryLiveFor(*ni))
        nodes.push_back(*ni);
    }
  } else {
    std::string err;
    if (!CollectTargetsFromArgs(argc, argv, &nodes, &err)) {
      Error("%s", err.c_str());
      return 1;
    }
  }

  RealDiskInterface disk_interface;
  for (Node* node : nodes) {
    DepsLog::Deps* deps = deps_log_.GetDeps(node);
    if (!deps) {
      printf("%s: deps not found\n", node->path().c_str());
      continue;
    }

    std::string err;
    TimeStamp mtime = disk_interface.Stat(node->path(), &err);
    if (mtime == -1)
      Error("%s", err.c_str()); // Log and ignore Stat() errors;
    printf(
        "%s: #deps %d, deps mtime %" PRId64 " (%s)\n", node->path().c_str(),
        deps->node_count, deps->mtime,
        (!mtime || mtime > deps->mtime ? "STALE" : "VALID")
    );
    for (int i = 0; i < deps->node_count; ++i)
      printf("    %s\n", deps->nodes[i]->path().c_str());
    printf("\n");
  }

  return 0;
}

int
NinjaMain::ToolMissingDeps(const Options* options, int argc, char** argv) {
  std::vector<Node*> nodes;
  std::string err;
  if (!CollectTargetsFromArgs(argc, argv, &nodes, &err)) {
    Error("%s", err.c_str());
    return 1;
  }
  RealDiskInterface disk_interface;
  MissingDependencyPrinter printer;
  MissingDependencyScanner scanner(
      &printer, &deps_log_, &state_, &disk_interface
  );
  for (std::vector<Node*>::iterator it = nodes.begin(); it != nodes.end();
       ++it) {
    scanner.ProcessNode(*it);
  }
  scanner.PrintStats();
  if (scanner.HadMissingDeps())
    return 3;
  return 0;
}

int
NinjaMain::ToolTargets(const Options* options, int argc, char* argv[]) {
  int depth = 1;
  if (argc >= 1) {
    std::string mode = argv[0];
    if (mode == "rule") {
      std::string rule;
      if (argc > 1)
        rule = argv[1];
      if (rule.empty())
        return ToolTargetsSourceList(&state_);
      else
        return ToolTargetsList(&state_, rule);
    } else if (mode == "depth") {
      if (argc > 1)
        depth = atoi(argv[1]);
    } else if (mode == "all") {
      return ToolTargetsList(&state_);
    } else {
      const char* suggestion =
          SpellcheckString(mode.c_str(), "rule", "depth", "all", nullptr);
      if (suggestion) {
        Error(
            "unknown target tool mode '%s', did you mean '%s'?", mode.c_str(),
            suggestion
        );
      } else {
        Error("unknown target tool mode '%s'", mode.c_str());
      }
      return 1;
    }
  }

  std::string err;
  std::vector<Node*> root_nodes = state_.RootNodes(&err);
  if (err.empty()) {
    return ToolTargetsList(root_nodes, depth, 0);
  } else {
    Error("%s", err.c_str());
    return 1;
  }
}

int
NinjaMain::ToolRules(const Options* options, int argc, char* argv[]) {
  // Parse options.

  // The rules tool uses getopt, and expects argv[0] to contain the name of
  // the tool, i.e. "rules".
  argc++;
  argv--;

  bool print_description = false;

  optind = 1;
  int opt;
  while ((opt = getopt(argc, argv, const_cast<char*>("hd"))) != -1) {
    switch (opt) {
      case 'd':
        print_description = true;
        break;
      case 'h':
      default:
        printf(
            "usage: ninja -t rules [options]\n"
            "\n"
            "options:\n"
            "  -d     also print the description of the rule\n"
            "  -h     print this message\n"
        );
        return 1;
    }
  }
  argv += optind;
  argc -= optind;

  // Print rules

  using Rules = std::map<std::string, const Rule*>;
  const Rules& rules = state_.bindings_.GetRules();
  for (Rules::const_iterator i = rules.begin(); i != rules.end(); ++i) {
    printf("%s", i->first.c_str());
    if (print_description) {
      const Rule* rule = i->second;
      const EvalString* description = rule->GetBinding("description");
      if (description != nullptr) {
        printf(": %s", description->Unparse().c_str());
      }
    }
    printf("\n");
  }
  return 0;
}

enum PrintCommandMode { PCM_Single, PCM_All };
void
PrintCommands(Edge* edge, EdgeSet* seen, PrintCommandMode mode) {
  if (!edge)
    return;
  if (!seen->insert(edge).second)
    return;

  if (mode == PCM_All) {
    for (Node* input : edge->inputs_)
      PrintCommands(input->in_edge(), seen, mode);
  }

  if (!edge->is_phony())
    puts(edge->EvaluateCommand().c_str());
}

int
NinjaMain::ToolCommands(const Options* options, int argc, char* argv[]) {
  // The commands tool uses getopt, and expects argv[0] to contain the name of
  // the tool, i.e. "commands".
  ++argc;
  --argv;

  PrintCommandMode mode = PCM_All;

  optind = 1;
  int opt;
  while ((opt = getopt(argc, argv, const_cast<char*>("hs"))) != -1) {
    switch (opt) {
      case 's':
        mode = PCM_Single;
        break;
      case 'h':
      default:
        printf(
            "usage: ninja -t commands [options] [targets]\n"
            "\n"
            "options:\n"
            "  -s     only print the final command to build [target], not the whole chain\n"
        );
        return 1;
    }
  }
  argv += optind;
  argc -= optind;

  std::vector<Node*> nodes;
  std::string err;
  if (!CollectTargetsFromArgs(argc, argv, &nodes, &err)) {
    Error("%s", err.c_str());
    return 1;
  }

  EdgeSet seen;
  for (std::vector<Node*>::iterator in = nodes.begin(); in != nodes.end(); ++in)
    PrintCommands((*in)->in_edge(), &seen, mode);

  return 0;
}

void
CollectInputs(
    Edge* edge, std::set<Edge*>* seen, std::vector<std::string>* result
) {
  if (!edge)
    return;
  if (!seen->insert(edge).second)
    return;

  for (Node* input : edge->inputs_)
    CollectInputs(input->in_edge(), seen, result);

  if (!edge->is_phony()) {
    edge->CollectInputs(true, result);
  }
}

int
NinjaMain::ToolInputs(const Options* options, int argc, char* argv[]) {
  // The inputs tool uses getopt, and expects argv[0] to contain the name of
  // the tool, i.e. "inputs".
  argc++;
  argv--;
  optind = 1;
  int opt;
  const option kLongOptions[] = {
      {"help", no_argument, nullptr, 'h'}, {nullptr, 0, nullptr, 0}};
  while ((opt = getopt_long(argc, argv, "h", kLongOptions, nullptr)) != -1) {
    switch (opt) {
      case 'h':
      default:
        // clang-format off
      printf(
"Usage '-t inputs [options] [targets]\n"
"\n"
"List all inputs used for a set of targets. Note that this includes\n"
"explicit, implicit and order-only inputs, but not validation ones.\n\n"
"Options:\n"
"  -h, --help   Print this message.\n");
        // clang-format on
        return 1;
    }
  }
  argv += optind;
  argc -= optind;

  std::vector<Node*> nodes;
  std::string err;
  if (!CollectTargetsFromArgs(argc, argv, &nodes, &err)) {
    Error("%s", err.c_str());
    return 1;
  }

  std::set<Edge*> seen;
  std::vector<std::string> result;
  for (std::vector<Node*>::iterator in = nodes.begin(); in != nodes.end(); ++in)
    CollectInputs((*in)->in_edge(), &seen, &result);

  // Make output deterministic by sorting then removing duplicates.
  std::sort(result.begin(), result.end());
  result.erase(std::unique(result.begin(), result.end()), result.end());

  for (size_t n = 0; n < result.size(); ++n)
    puts(result[n].c_str());

  return 0;
}

int
NinjaMain::ToolClean(const Options* options, int argc, char* argv[]) {
  // The clean tool uses getopt, and expects argv[0] to contain the name of
  // the tool, i.e. "clean".
  argc++;
  argv--;

  bool generator = false;
  bool clean_rules = false;

  optind = 1;
  int opt;
  while ((opt = getopt(argc, argv, const_cast<char*>("hgr"))) != -1) {
    switch (opt) {
      case 'g':
        generator = true;
        break;
      case 'r':
        clean_rules = true;
        break;
      case 'h':
      default:
        printf(
            "usage: ninja -t clean [options] [targets]\n"
            "\n"
            "options:\n"
            "  -g     also clean files marked as ninja generator output\n"
            "  -r     interpret targets as a list of rules to clean instead\n"
        );
        return 1;
    }
  }
  argv += optind;
  argc -= optind;

  if (clean_rules && argc == 0) {
    Error("expected a rule to clean");
    return 1;
  }

  Cleaner cleaner(&state_, config_, &disk_interface_);
  if (argc >= 1) {
    if (clean_rules)
      return cleaner.CleanRules(argc, argv);
    else
      return cleaner.CleanTargets(argc, argv);
  } else {
    return cleaner.CleanAll(generator);
  }
}

int
NinjaMain::ToolCleanDead(const Options* options, int argc, char* argv[]) {
  Cleaner cleaner(&state_, config_, &disk_interface_);
  return cleaner.CleanDead(build_log_.entries());
}

enum EvaluateCommandMode { ECM_NORMAL, ECM_EXPAND_RSPFILE };
std::string
EvaluateCommandWithRspfile(const Edge* edge, const EvaluateCommandMode mode) {
  std::string command = edge->EvaluateCommand();
  if (mode == ECM_NORMAL)
    return command;

  std::string rspfile = edge->GetUnescapedRspfile();
  if (rspfile.empty())
    return command;

  size_t index = command.find(rspfile);
  if (index == 0 || index == std::string::npos || command[index - 1] != '@')
    return command;

  std::string rspfile_content = edge->GetBinding("rspfile_content");
  size_t newline_index = 0;
  while ((newline_index = rspfile_content.find('\n', newline_index))
         != std::string::npos) {
    rspfile_content.replace(newline_index, 1, 1, ' ');
    ++newline_index;
  }
  command.replace(index - 1, rspfile.length() + 1, rspfile_content);
  return command;
}

void
printCompdb(
    const char* const directory, const Edge* const edge,
    const EvaluateCommandMode eval_mode
) {
  printf("\n  {\n    \"directory\": \"");
  PrintJSONString(directory);
  printf("\",\n    \"command\": \"");
  PrintJSONString(EvaluateCommandWithRspfile(edge, eval_mode));
  printf("\",\n    \"file\": \"");
  PrintJSONString(edge->inputs_[0]->path());
  printf("\",\n    \"output\": \"");
  PrintJSONString(edge->outputs_[0]->path());
  printf("\"\n  }");
}

int
NinjaMain::ToolCompilationDatabase(
    const Options* options, int argc, char* argv[]
) {
  // The compdb tool uses getopt, and expects argv[0] to contain the name of
  // the tool, i.e. "compdb".
  argc++;
  argv--;

  EvaluateCommandMode eval_mode = ECM_NORMAL;

  optind = 1;
  int opt;
  while ((opt = getopt(argc, argv, const_cast<char*>("hx"))) != -1) {
    switch (opt) {
      case 'x':
        eval_mode = ECM_EXPAND_RSPFILE;
        break;

      case 'h':
      default:
        printf(
            "usage: ninja -t compdb [options] [rules]\n"
            "\n"
            "options:\n"
            "  -x     expand @rspfile style response file invocations\n"
        );
        return 1;
    }
  }
  argv += optind;
  argc -= optind;

  bool first = true;
  std::vector<char> cwd;
  char* success = nullptr;

  do {
    cwd.resize(cwd.size() + 1024);
    errno = 0;
    success = getcwd(&cwd[0], cwd.size());
  } while (!success && errno == ERANGE);
  if (!success) {
    Error("cannot determine working directory: %s", strerror(errno));
    return 1;
  }

  putchar('[');
  for (std::unique_ptr<Edge>& edge : state_.edges_) {
    if (edge->inputs_.empty())
      continue;
    if (argc == 0) {
      if (!first) {
        putchar(',');
      }
      printCompdb(&cwd[0], edge.get(), eval_mode);
      first = false;
    } else {
      for (int i = 0; i != argc; ++i) {
        if (edge->rule_->name() == argv[i]) {
          if (!first) {
            putchar(',');
          }
          printCompdb(&cwd[0], edge.get(), eval_mode);
          first = false;
        }
      }
    }
  }

  puts("\n]");
  return 0;
}

int
NinjaMain::ToolRecompact(const Options* options, int argc, char* argv[]) {
  if (!EnsureBuildDirExists())
    return 1;

  if (!OpenBuildLog(/*recompact_only=*/true)
      || !OpenDepsLog(/*recompact_only=*/true))
    return 1;

  return 0;
}

int
NinjaMain::ToolRestat(const Options* options, int argc, char* argv[]) {
  // The restat tool uses getopt, and expects argv[0] to contain the name of the
  // tool, i.e. "restat"
  argc++;
  argv--;

  optind = 1;
  int opt;
  while ((opt = getopt(argc, argv, const_cast<char*>("h"))) != -1) {
    switch (opt) {
      case 'h':
      default:
        printf("usage: ninja -t restat [outputs]\n");
        return 1;
    }
  }
  argv += optind;
  argc -= optind;

  if (!EnsureBuildDirExists())
    return 1;

  std::string log_path = ".ninja_log";
  if (!build_dir_.empty())
    log_path = build_dir_ + "/" + log_path;

  std::string err;
  const LoadStatus status = build_log_.Load(log_path, &err);
  if (status == LOAD_ERROR) {
    Error("loading build log %s: %s", log_path.c_str(), err.c_str());
    return EXIT_FAILURE;
  }
  if (status == LOAD_NOT_FOUND) {
    // Nothing to restat, ignore this
    return EXIT_SUCCESS;
  }
  if (!err.empty()) {
    // Hack: Load() can return a warning via err by returning LOAD_SUCCESS.
    Warning("%s", err.c_str());
    err.clear();
  }

  bool success = build_log_.Restat(log_path, disk_interface_, argc, argv, &err);
  if (!success) {
    Error("failed recompaction: %s", err.c_str());
    return EXIT_FAILURE;
  }

  if (!config_.dry_run) {
    if (!build_log_.OpenForWrite(log_path, *this, &err)) {
      Error("opening build log: %s", err.c_str());
      return EXIT_FAILURE;
    }
  }

  return EXIT_SUCCESS;
}

int
NinjaMain::ToolUrtle(const Options* options, int argc, char** argv) {
  // RLE encoded.
  const char* urtle =
      " 13 ,3;2!2;\n8 ,;<11!;\n5 `'<10!(2`'2!\n11 ,6;, `\\. `\\9 .,c13$ec,.\n6 "
      ",2;11!>; `. ,;!2> .e8$2\".2 \"?7$e.\n <:<8!'` 2.3,.2` ,3!' ;,(?7\";2!2'<"
      "; `?6$PF ,;,\n2 `'4!8;<!3'`2 3! ;,`'2`2'3!;4!`2.`!;2 3,2 .<!2'`).\n5 3`5"
      "'2`9 `!2 `4!><3;5! J2$b,`!>;2!:2!`,d?b`!>\n26 `'-;,(<9!> $F3 )3.:!.2 d\""
      "2 ) !>\n30 7`2'<3!- \"=-='5 .2 `2-=\",!>\n25 .ze9$er2 .,cd16$bc.'\n22 .e"
      "14$,26$.\n21 z45$c .\n20 J50$c\n20 14$P\"`?34$b\n20 14$ dbc `2\"?22$?7$c"
      "\n20 ?18$c.6 4\"8?4\" c8$P\n9 .2,.8 \"20$c.3 ._14 J9$\n .2,2c9$bec,.2 `?"
      "21$c.3`4%,3%,3 c8$P\"\n22$c2 2\"?21$bc2,.2` .2,c7$P2\",cb\n23$b bc,.2\"2"
      "?14$2F2\"5?2\",J5$P\" ,zd3$\n24$ ?$3?%3 `2\"2?12$bcucd3$P3\"2 2=7$\n23$P"
      "\" ,3;<5!>2;,. `4\"6?2\"2 ,9;, `\"?2$\n";
  int count = 0;
  for (const char* p = urtle; *p; p++) {
    if ('0' <= *p && *p <= '9') {
      count = count * 10 + *p - '0';
    } else {
      for (int i = 0; i < std::max(count, 1); ++i)
        printf("%c", *p);
      count = 0;
    }
  }
  return 0;
}

/// Find the function to execute for \a tool_name and return it via \a func.
/// Returns a Tool, or nullptr if Ninja should exit.
const Tool*
ChooseTool(const std::string& tool_name) {
  static const Tool kTools[] = {
      {"browse", "browse dependency graph in a web browser",
       Tool::RUN_AFTER_LOAD, &NinjaMain::ToolBrowse},
      {"clean", "clean built files", Tool::RUN_AFTER_LOAD,
       &NinjaMain::ToolClean},
      {"commands", "list all commands required to rebuild given targets",
       Tool::RUN_AFTER_LOAD, &NinjaMain::ToolCommands},
      {"inputs", "list all inputs required to rebuild given targets",
       Tool::RUN_AFTER_LOAD, &NinjaMain::ToolInputs},
      {"deps", "show dependencies stored in the deps log", Tool::RUN_AFTER_LOGS,
       &NinjaMain::ToolDeps},
      {"missingdeps", "check deps log dependencies on generated files",
       Tool::RUN_AFTER_LOGS, &NinjaMain::ToolMissingDeps},
      {"graph", "output graphviz dot file for targets", Tool::RUN_AFTER_LOAD,
       &NinjaMain::ToolGraph},
      {"query", "show inputs/outputs for a path", Tool::RUN_AFTER_LOGS,
       &NinjaMain::ToolQuery},
      {"targets", "list targets by their rule or depth in the DAG",
       Tool::RUN_AFTER_LOAD, &NinjaMain::ToolTargets},
      {"compdb", "dump JSON compilation database to stdout",
       Tool::RUN_AFTER_LOAD, &NinjaMain::ToolCompilationDatabase},
      {"recompact", "recompacts ninja-internal data structures",
       Tool::RUN_AFTER_LOAD, &NinjaMain::ToolRecompact},
      {"restat", "restats all outputs in the build log", Tool::RUN_AFTER_FLAGS,
       &NinjaMain::ToolRestat},
      {"rules", "list all rules", Tool::RUN_AFTER_LOAD, &NinjaMain::ToolRules},
      {"cleandead",
       "clean built files that are no longer produced by the manifest",
       Tool::RUN_AFTER_LOGS, &NinjaMain::ToolCleanDead},
      {"urtle", nullptr, Tool::RUN_AFTER_FLAGS, &NinjaMain::ToolUrtle},
      {nullptr, nullptr, Tool::RUN_AFTER_FLAGS, nullptr}};

  if (tool_name == "list") {
    printf("ninja subtools:\n");
    for (const Tool* tool = &kTools[0]; tool->name; ++tool) {
      if (tool->desc)
        printf("%11s  %s\n", tool->name, tool->desc);
    }
    return nullptr;
  }

  for (const Tool* tool = &kTools[0]; tool->name; ++tool) {
    if (tool->name == tool_name)
      return tool;
  }

  std::vector<const char*> words;
  for (const Tool* tool = &kTools[0]; tool->name; ++tool)
    words.push_back(tool->name);
  const char* suggestion = SpellcheckStringV(tool_name, words);
  if (suggestion) {
    Fatal(
        "unknown tool '%s', did you mean '%s'?", tool_name.c_str(), suggestion
    );
  } else {
    Fatal("unknown tool '%s'", tool_name.c_str());
  }
  return nullptr; // Not reached.
}

/// Enable a debugging mode.  Returns false if Ninja should exit instead
/// of continuing.
bool
DebugEnable(const std::string& name) {
  if (name == "list") {
    printf(
        "debugging modes:\n"
        "  stats        print operation counts/timing info\n"
        "  explain      explain what caused a command to execute\n"
        "  keepdepfile  don't delete depfiles after they're read by ninja\n"
        "  keeprsp      don't delete @response files on success\n"
        "multiple modes can be enabled via -d FOO -d BAR\n"
    );
    return false;
  } else if (name == "stats") {
    g_metrics = new Metrics;
    return true;
  } else if (name == "explain") {
    g_explaining = true;
    return true;
  } else if (name == "keepdepfile") {
    g_keep_depfile = true;
    return true;
  } else if (name == "keeprsp") {
    g_keep_rsp = true;
    return true;
  } else if (name == "nostatcache") {
    g_experimental_statcache = false;
    return true;
  } else {
    const char* suggestion = SpellcheckString(
        name.c_str(), "stats", "explain", "keepdepfile", "keeprsp",
        "nostatcache", nullptr
    );
    if (suggestion) {
      Error(
          "unknown debug setting '%s', did you mean '%s'?", name.c_str(),
          suggestion
      );
    } else {
      Error("unknown debug setting '%s'", name.c_str());
    }
    return false;
  }
}

/// Set a warning flag.  Returns false if Ninja should exit instead of
/// continuing.
bool
WarningEnable(const std::string& name, Options* options) {
  if (name == "list") {
    printf(
        "warning flags:\n"
        "  phonycycle={err,warn}  phony build statement references itself\n"
    );
    return false;
  } else if (name == "dupbuild=err") {
    options->dupe_edges_should_err = true;
    return true;
  } else if (name == "dupbuild=warn") {
    options->dupe_edges_should_err = false;
    return true;
  } else if (name == "phonycycle=err") {
    options->phony_cycle_should_err = true;
    return true;
  } else if (name == "phonycycle=warn") {
    options->phony_cycle_should_err = false;
    return true;
  } else if (name == "depfilemulti=err" || name == "depfilemulti=warn") {
    Warning("deprecated warning 'depfilemulti'");
    return true;
  } else {
    const char* suggestion = SpellcheckString(
        name.c_str(), "dupbuild=err", "dupbuild=warn", "phonycycle=err",
        "phonycycle=warn", nullptr
    );
    if (suggestion) {
      Error(
          "unknown warning flag '%s', did you mean '%s'?", name.c_str(),
          suggestion
      );
    } else {
      Error("unknown warning flag '%s'", name.c_str());
    }
    return false;
  }
}

bool
NinjaMain::OpenBuildLog(bool recompact_only) {
  std::string log_path = ".ninja_log";
  if (!build_dir_.empty())
    log_path = build_dir_ + "/" + log_path;

  std::string err;
  const LoadStatus status = build_log_.Load(log_path, &err);
  if (status == LOAD_ERROR) {
    Error("loading build log %s: %s", log_path.c_str(), err.c_str());
    return false;
  }
  if (!err.empty()) {
    // Hack: Load() can return a warning via err by returning LOAD_SUCCESS.
    Warning("%s", err.c_str());
    err.clear();
  }

  if (recompact_only) {
    if (status == LOAD_NOT_FOUND) {
      return true;
    }
    bool success = build_log_.Recompact(log_path, *this, &err);
    if (!success)
      Error("failed recompaction: %s", err.c_str());
    return success;
  }

  if (!config_.dry_run) {
    if (!build_log_.OpenForWrite(log_path, *this, &err)) {
      Error("opening build log: %s", err.c_str());
      return false;
    }
  }

  return true;
}

/// Open the deps log: load it, then open for writing.
/// @return false on error.
bool
NinjaMain::OpenDepsLog(bool recompact_only) {
  std::string path = ".ninja_deps";
  if (!build_dir_.empty())
    path = build_dir_ + "/" + path;

  std::string err;
  const LoadStatus status = deps_log_.Load(path, &state_, &err);
  if (status == LOAD_ERROR) {
    Error("loading deps log %s: %s", path.c_str(), err.c_str());
    return false;
  }
  if (!err.empty()) {
    // Hack: Load() can return a warning via err by returning LOAD_SUCCESS.
    Warning("%s", err.c_str());
    err.clear();
  }

  if (recompact_only) {
    if (status == LOAD_NOT_FOUND) {
      return true;
    }
    bool success = deps_log_.Recompact(path, &err);
    if (!success)
      Error("failed recompaction: %s", err.c_str());
    return success;
  }

  if (!config_.dry_run) {
    if (!deps_log_.OpenForWrite(path, &err)) {
      Error("opening deps log: %s", err.c_str());
      return false;
    }
  }

  return true;
}

void
NinjaMain::DumpMetrics() {
  g_metrics->Report();

  printf("\n");
  int count = (int)state_.paths_.size();
  int buckets = (int)state_.paths_.bucket_count();
  printf(
      "path->node hash load %.2f (%d entries / %d buckets)\n",
      count / (double)buckets, count, buckets
  );
}

bool
NinjaMain::EnsureBuildDirExists() {
  build_dir_ = state_.bindings_.LookupVariable("builddir");
  if (!build_dir_.empty() && !config_.dry_run) {
    if (!disk_interface_.MakeDirs(build_dir_ + "/.") && errno != EEXIST) {
      Error(
          "creating build directory %s: %s", build_dir_.c_str(), strerror(errno)
      );
      return false;
    }
  }
  return true;
}

int
NinjaMain::RunBuild(int argc, char** argv, Status* status) {
  std::string err;
  std::vector<Node*> targets;
  if (!CollectTargetsFromArgs(argc, argv, &targets, &err)) {
    status->Error("%s", err.c_str());
    return 1;
  }

  Builder builder(
      &state_, config_, &build_log_, &deps_log_, &disk_interface_, status,
      start_time_millis_
  );
  for (size_t i = 0; i < targets.size(); ++i) {
    if (!builder.AddTarget(targets[i], &err)) {
      if (!err.empty()) {
        status->Error("%s", err.c_str());
        return 1;
      } else {
        // Added a target that is already up-to-date; not really
        // an error.
      }
    }
  }

  if (builder.AlreadyUpToDate()) {
    status->Info("no work to do.");
    return 0;
  }

  if (!builder.Build(&err)) {
    status->Info("build stopped: %s.", err.c_str());
    if (err.find("interrupted by user") != std::string::npos) {
      return 2;
    }
    return 1;
  }

  return 0;
}

#ifdef _MSC_VER

/// This handler processes fatal crashes that you can't catch
/// Test example: C++ exception in a stack-unwind-block
/// Real-world example: ninja launched a compiler to process a tricky
/// C++ input file. The compiler got itself into a state where it
/// generated 3 GB of output and caused ninja to crash.
void
TerminateHandler() {
  CreateWin32MiniDump(nullptr);
  Fatal("terminate handler called");
}

/// On Windows, we want to prevent error dialogs in case of exceptions.
/// This function handles the exception, and writes a minidump.
int
ExceptionFilter(unsigned int code, struct _EXCEPTION_POINTERS* ep) {
  Error("exception: 0x%X", code); // e.g. EXCEPTION_ACCESS_VIOLATION
  fflush(stderr);
  CreateWin32MiniDump(ep);
  return EXCEPTION_EXECUTE_HANDLER;
}

#endif // _MSC_VER

class DeferGuessParallelism {
public:
  bool needGuess;
  BuildConfig* config;

  DeferGuessParallelism(BuildConfig* config)
      : needGuess(true), config(config) {}

  void
  Refresh() {
    if (needGuess) {
      needGuess = false;
      config->parallelism = GuessParallelism();
    }
  }
  ~DeferGuessParallelism() { Refresh(); }
};

/// Parse argv for command-line options.
/// Returns an exit code, or -1 if Ninja should continue.
int
ReadFlags(int* argc, char*** argv, Options* options, BuildConfig* config) {
  DeferGuessParallelism deferGuessParallelism(config);

  enum { OPT_VERSION = 1, OPT_QUIET = 2 };
  const option kLongOptions[] = {
      {"help", no_argument, nullptr, 'h'},
      {"version", no_argument, nullptr, OPT_VERSION},
      {"verbose", no_argument, nullptr, 'v'},
      {"quiet", no_argument, nullptr, OPT_QUIET},
      {nullptr, 0, nullptr, 0}};

  int opt;
  while (!options->tool
         && (opt = getopt_long(
                 *argc, *argv, "d:f:j:k:l:nt:vw:C:h", kLongOptions, nullptr
             )) != -1) {
    switch (opt) {
      case 'd':
        if (!DebugEnable(optarg))
          return 1;
        break;
      case 'f':
        options->input_file = optarg;
        break;
      case 'j': {
        char* end;
        int value = strtol(optarg, &end, 10);
        if (*end != 0 || value < 0)
          Fatal("invalid -j parameter");

        // We want to run N jobs in parallel. For N = 0, INT_MAX
        // is close enough to infinite for most sane builds.
        config->parallelism = value > 0 ? value : INT_MAX;
        deferGuessParallelism.needGuess = false;
        break;
      }
      case 'k': {
        char* end;
        int value = strtol(optarg, &end, 10);
        if (*end != 0)
          Fatal("-k parameter not numeric; did you mean -k 0?");

        // We want to go until N jobs fail, which means we should allow
        // N failures and then stop.  For N <= 0, INT_MAX is close enough
        // to infinite for most sane builds.
        config->failures_allowed = value > 0 ? value : INT_MAX;
        break;
      }
      case 'l': {
        char* end;
        double value = strtod(optarg, &end);
        if (end == optarg)
          Fatal("-l parameter not numeric: did you mean -l 0.0?");
        config->max_load_average = value;
        break;
      }
      case 'n':
        config->dry_run = true;
        break;
      case 't':
        options->tool = ChooseTool(optarg);
        if (!options->tool)
          return 0;
        break;
      case 'v':
        config->verbosity = BuildConfig::VERBOSE;
        break;
      case OPT_QUIET:
        config->verbosity = BuildConfig::NO_STATUS_UPDATE;
        break;
      case 'w':
        if (!WarningEnable(optarg, options))
          return 1;
        break;
      case 'C':
        options->working_dir = optarg;
        break;
      case OPT_VERSION:
        printf("%s\n", kNinjaVersion);
        return 0;
      case 'h':
      default:
        deferGuessParallelism.Refresh();
        Usage(*config);
        return 1;
    }
  }
  *argv += optind;
  *argc -= optind;

  return -1;
}

NORETURN void
real_main(int argc, char** argv) {
  // Use exit() instead of return in this function to avoid potentially
  // expensive cleanup when destructing NinjaMain.
  BuildConfig config;
  Options options = {};
  options.input_file = "build.ninja";
  options.dupe_edges_should_err = true;

  setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
  const char* ninja_command = argv[0];

  int exit_code = ReadFlags(&argc, &argv, &options, &config);
  if (exit_code >= 0)
    exit(exit_code);

  Status* status = new StatusPrinter(config);

  if (options.working_dir) {
    // The formatting of this string, complete with funny quotes, is
    // so Emacs can properly identify that the cwd has changed for
    // subsequent commands.
    // Don't print this if a tool is being used, so that tool output
    // can be piped into a file without this string showing up.
    if (!options.tool && config.verbosity != BuildConfig::NO_STATUS_UPDATE)
      status->Info("Entering directory `%s'", options.working_dir);
    if (chdir(options.working_dir) < 0) {
      Fatal("chdir to '%s' - %s", options.working_dir, strerror(errno));
    }
  }

  if (options.tool && options.tool->when == Tool::RUN_AFTER_FLAGS) {
    // None of the RUN_AFTER_FLAGS actually use a NinjaMain, but it's needed
    // by other tools.
    NinjaMain ninja(ninja_command, config);
    exit((ninja.*options.tool->func)(&options, argc, argv));
  }

  // Limit number of rebuilds, to prevent infinite loops.
  const int kCycleLimit = 100;
  for (int cycle = 1; cycle <= kCycleLimit; ++cycle) {
    NinjaMain ninja(ninja_command, config);

    ManifestParserOptions parser_opts;
    if (options.dupe_edges_should_err) {
      parser_opts.dupe_edge_action_ = kDupeEdgeActionError;
    }
    if (options.phony_cycle_should_err) {
      parser_opts.phony_cycle_action_ = kPhonyCycleActionError;
    }
    ManifestParser parser(&ninja.state_, &ninja.disk_interface_, parser_opts);
    std::string err;
    if (!parser.Load(options.input_file, &err)) {
      status->Error("%s", err.c_str());
      exit(1);
    }

    if (options.tool && options.tool->when == Tool::RUN_AFTER_LOAD)
      exit((ninja.*options.tool->func)(&options, argc, argv));

    if (!ninja.EnsureBuildDirExists())
      exit(1);

    if (!ninja.OpenBuildLog() || !ninja.OpenDepsLog())
      exit(1);

    if (options.tool && options.tool->when == Tool::RUN_AFTER_LOGS)
      exit((ninja.*options.tool->func)(&options, argc, argv));

    // Attempt to rebuild the manifest before building anything else
    if (ninja.RebuildManifest(options.input_file, &err, status)) {
      // In dry_run mode the regeneration will succeed without changing the
      // manifest forever. Better to return immediately.
      if (config.dry_run)
        exit(0);
      // Start the build over with the new manifest.
      continue;
    } else if (!err.empty()) {
      status->Error("rebuilding '%s': %s", options.input_file, err.c_str());
      exit(1);
    }

    int result = ninja.RunBuild(argc, argv, status);
    if (g_metrics)
      ninja.DumpMetrics();
    exit(result);
  }

  status->Error(
      "manifest '%s' still dirty after %d tries, perhaps system time is not set",
      options.input_file, kCycleLimit
  );
  exit(1);
}

} // anonymous namespace

int
main(int argc, char** argv) {
#if defined(_MSC_VER)
  // Set a handler to catch crashes not caught by the __try..__except
  // block (e.g. an exception in a stack-unwind-block).
  std::set_terminate(TerminateHandler);
  __try {
    // Running inside __try ... __except suppresses any Windows error
    // dialogs for errors such as bad_alloc.
    real_main(argc, argv);
  } __except (ExceptionFilter(GetExceptionCode(), GetExceptionInformation())) {
    // Common error situations return exitCode=1. 2 was chosen to
    // indicate a more serious problem.
    return 2;
  }
#else
  real_main(argc, argv);
#endif
}
