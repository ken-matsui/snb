// Copyright 2016 Google Inc. All Rights Reserved.
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

#include <cstdarg>
#include <cstdlib>
#include <ninja/debug_flags.hpp>
#include <ninja/status.hpp>

StatusPrinter::StatusPrinter(const BuildConfig& config)
    : config_(config), started_edges_(0), finished_edges_(0), total_edges_(0),
      running_edges_(0), time_millis_(0), progress_status_format_(nullptr),
      current_rate_(config.parallelism) {

  // Don't do anything fancy in verbose mode.
  if (config_.verbosity != BuildConfig::NORMAL)
    printer_.set_smart_terminal(false);

  progress_status_format_ = getenv("NINJA_STATUS");
  if (!progress_status_format_)
    progress_status_format_ = "[%f/%t] ";
}

void
StatusPrinter::PlanHasTotalEdges(int total) {
  total_edges_ = total;
}

void
StatusPrinter::BuildEdgeStarted(const Edge* edge, int64_t start_time_millis) {
  ++started_edges_;
  ++running_edges_;
  time_millis_ = start_time_millis;

  if (edge->use_console() || printer_.is_smart_terminal())
    PrintStatus(edge, start_time_millis);

  if (edge->use_console())
    printer_.SetConsoleLocked(true);
}

void
StatusPrinter::BuildEdgeFinished(
    Edge* edge, int64_t end_time_millis, bool success, const std::string& output
) {
  time_millis_ = end_time_millis;
  ++finished_edges_;

  if (edge->use_console())
    printer_.SetConsoleLocked(false);

  if (config_.verbosity == BuildConfig::QUIET)
    return;

  if (!edge->use_console())
    PrintStatus(edge, end_time_millis);

  --running_edges_;

  // Print the command that is spewing before printing its output.
  if (!success) {
    std::string outputs;
    for (std::vector<Node*>::const_iterator o = edge->outputs_.begin();
         o != edge->outputs_.end(); ++o)
      outputs += (*o)->path() + " ";

    if (printer_.supports_color()) {
      printer_.PrintOnNewLine(
          "\x1B[31m"
          "FAILED: "
          "\x1B[0m"
          + outputs + "\n"
      );
    } else {
      printer_.PrintOnNewLine("FAILED: " + outputs + "\n");
    }
    printer_.PrintOnNewLine(edge->EvaluateCommand() + "\n");
  }

  if (!output.empty()) {
    // ninja sets stdout and stderr of subprocesses to a pipe, to be able to
    // check if the output is empty. Some compilers, e.g. clang, check
    // isatty(stderr) to decide if they should print colored output.
    // To make it possible to use colored output with ninja, subprocesses should
    // be run with a flag that forces them to always print color escape codes.
    // To make sure these escape codes don't show up in a file if ninja's output
    // is piped to a file, ninja strips ansi escape codes again if it's not
    // writing to a |smart_terminal_|.
    // (Launching subprocesses in pseudo ttys doesn't work because there are
    // only a few hundred available on some systems, and ninja can launch
    // thousands of parallel compile commands.)
    std::string final_output;
    if (!printer_.supports_color())
      final_output = StripAnsiEscapeCodes(output);
    else
      final_output = output;

    printer_.PrintOnNewLine(final_output);
  }
}

void
StatusPrinter::BuildLoadDyndeps() {
  // The DependencyScan calls EXPLAIN() to print lines explaining why
  // it considers a portion of the graph to be out of date.  Normally
  // this is done before the build starts, but our caller is about to
  // load a dyndep file during the build.  Doing so may generate more
  // explanation lines (via fprintf directly to stderr), but in an
  // interactive console the cursor is currently at the end of a status
  // line.  Start a new line so that the first explanation does not
  // append to the status line.  After the explanations are done a
  // new build status line will appear.
  if (g_explaining)
    printer_.PrintOnNewLine("");
}

void
StatusPrinter::BuildStarted() {
  started_edges_ = 0;
  finished_edges_ = 0;
  running_edges_ = 0;
}

void
StatusPrinter::BuildFinished() {
  printer_.SetConsoleLocked(false);
  printer_.PrintOnNewLine("");
}

std::string
StatusPrinter::FormatProgressStatus(
    const char* progress_status_format, int64_t time_millis
) const {
  std::string out;
  char buf[32];
  for (const char* s = progress_status_format; *s != '\0'; ++s) {
    if (*s == '%') {
      ++s;
      switch (*s) {
        case '%':
          out.push_back('%');
          break;

          // Started edges.
        case 's':
          snprintf(buf, sizeof(buf), "%d", started_edges_);
          out += buf;
          break;

          // Total edges.
        case 't':
          snprintf(buf, sizeof(buf), "%d", total_edges_);
          out += buf;
          break;

          // Running edges.
        case 'r': {
          snprintf(buf, sizeof(buf), "%d", running_edges_);
          out += buf;
          break;
        }

          // Unstarted edges.
        case 'u':
          snprintf(buf, sizeof(buf), "%d", total_edges_ - started_edges_);
          out += buf;
          break;

          // Finished edges.
        case 'f':
          snprintf(buf, sizeof(buf), "%d", finished_edges_);
          out += buf;
          break;

          // Overall finished edges per second.
        case 'o':
          SnprintfRate(finished_edges_ / (time_millis / 1e3), buf, "%.1f");
          out += buf;
          break;

          // Current rate, average over the last '-j' jobs.
        case 'c':
          current_rate_.UpdateRate(finished_edges_, time_millis);
          SnprintfRate(current_rate_.rate(), buf, "%.1f");
          out += buf;
          break;

          // Percentage
        case 'p': {
          int percent = (100 * finished_edges_) / total_edges_;
          snprintf(buf, sizeof(buf), "%3i%%", percent);
          out += buf;
          break;
        }

        case 'e': {
          snprintf(buf, sizeof(buf), "%.3f", time_millis / 1e3);
          out += buf;
          break;
        }

        default:
          Fatal("unknown placeholder '%%%c' in $NINJA_STATUS", *s);
          return "";
      }
    } else {
      out.push_back(*s);
    }
  }

  return out;
}

void
StatusPrinter::PrintStatus(const Edge* edge, int64_t time_millis) {
  if (config_.verbosity == BuildConfig::QUIET
      || config_.verbosity == BuildConfig::NO_STATUS_UPDATE)
    return;

  bool force_full_command = config_.verbosity == BuildConfig::VERBOSE;

  std::string to_print = edge->GetBinding("description");
  if (to_print.empty() || force_full_command)
    to_print = edge->GetBinding("command");

  to_print =
      FormatProgressStatus(progress_status_format_, time_millis) + to_print;

  printer_.Print(
      to_print, force_full_command ? LinePrinter::FULL : LinePrinter::ELIDE
  );
}

void
StatusPrinter::Warning(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);
  ::Warning(msg, ap);
  va_end(ap);
}

void
StatusPrinter::Error(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);
  ::Error(msg, ap);
  va_end(ap);
}

void
StatusPrinter::Info(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);
  ::Info(msg, ap);
  va_end(ap);
}
