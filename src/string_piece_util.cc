// Copyright 2017 Google Inc. All Rights Reserved.
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
#include <ninja/string_piece_util.hpp>
#include <string>
#include <vector>

std::vector<std::string_view>
SplitStringPiece(std::string_view input, char sep) {
  std::vector<std::string_view> elems;
  elems.reserve(std::count(input.begin(), input.end(), sep) + 1);

  std::string_view::const_iterator pos = input.begin();

  for (;;) {
    const char* next_pos = std::find(pos, input.end(), sep);
    if (next_pos == input.end()) {
      elems.emplace_back(pos, input.end() - pos);
      break;
    }
    elems.emplace_back(pos, next_pos - pos);
    pos = next_pos + 1;
  }

  return elems;
}

std::string
JoinStringPiece(const std::vector<std::string_view>& list, char sep) {
  if (list.empty()) {
    return "";
  }

  std::string ret;

  {
    size_t cap = list.size() - 1;
    for (size_t i = 0; i < list.size(); ++i) {
      cap += list[i].size();
    }
    ret.reserve(cap);
  }

  for (size_t i = 0; i < list.size(); ++i) {
    if (i != 0) {
      ret += sep;
    }
    ret.append(list[i].data(), list[i].size());
  }

  return ret;
}

bool
EqualsCaseInsensitiveASCII(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) {
    return false;
  }

  for (size_t i = 0; i < a.size(); ++i) {
    if (ToLowerASCII(a[i]) != ToLowerASCII(b[i])) {
      return false;
    }
  }

  return true;
}
