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

#ifndef NINJA_EDIT_DISTANCE_H_
#define NINJA_EDIT_DISTANCE_H_

#include <string_view>

int EditDistance(const std::string_view& s1,
                 const std::string_view& s2,
                 bool allow_replacements = true,
                 int max_edit_distance = 0);

#endif  // NINJA_EDIT_DISTANCE_H_
