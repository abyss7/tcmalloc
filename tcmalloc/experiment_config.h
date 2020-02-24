// Copyright 2019 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_EXPERIMENT_CONFIG_H_
#define TCMALLOC_EXPERIMENT_CONFIG_H_

namespace tcmalloc {

enum class Experiment : int {
  TCMALLOC_TEMERAIRE,
  TCMALLOC_SANS_56_SIZECLASS,
  TCMALLOC_ARBITRARY_TRANSFER_CACHE,
  TCMALLOC_LARGE_NUM_TO_MOVE,
  kMaxExperimentID,
};

struct ExperimentConfig {
  Experiment id;
  const char* name;
};

// TODO(b/134687226): Consider making this programmatically generated
constexpr ExperimentConfig experiments[] = {
    {Experiment::TCMALLOC_TEMERAIRE, "TCMALLOC_TEMERAIRE"},
    {Experiment::TCMALLOC_SANS_56_SIZECLASS, "TCMALLOC_SANS_56_SIZECLASS"},
    {Experiment::TCMALLOC_ARBITRARY_TRANSFER_CACHE,
     "TCMALLOC_ARBITRARY_TRANSFER_CACHE"},
    {Experiment::TCMALLOC_LARGE_NUM_TO_MOVE, "TCMALLOC_LARGE_NUM_TO_MOVE"},
};

}  // namespace tcmalloc

#endif  // TCMALLOC_EXPERIMENT_CONFIG_H_
