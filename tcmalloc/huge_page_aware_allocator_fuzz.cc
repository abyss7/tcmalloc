// Copyright 2022 The TCMalloc Authors
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

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "absl/log/check.h"
#include "tcmalloc/huge_page_aware_allocator.h"
#include "tcmalloc/lifetime_based_allocator.h"
#include "tcmalloc/pages.h"
#include "tcmalloc/span.h"

namespace {
using tcmalloc::tcmalloc_internal::BackingStats;
using tcmalloc::tcmalloc_internal::HugePageAwareAllocator;
using tcmalloc::tcmalloc_internal::HugeRegionCountOption;
using tcmalloc::tcmalloc_internal::kNumaPartitions;
using tcmalloc::tcmalloc_internal::kPagesPerHugePage;
using tcmalloc::tcmalloc_internal::kTop;
using tcmalloc::tcmalloc_internal::Length;
using tcmalloc::tcmalloc_internal::LifetimePredictionOptions;
using tcmalloc::tcmalloc_internal::MemoryTag;
using tcmalloc::tcmalloc_internal::pageheap_lock;
using tcmalloc::tcmalloc_internal::PbtxtRegion;
using tcmalloc::tcmalloc_internal::Printer;
using tcmalloc::tcmalloc_internal::Span;
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size < 13 || size > 100000) {
    // size < 13 for needing some entropy to initialize huge page aware
    // allocator.
    //
    // size > 100000 for avoiding overly large inputs given we do extra
    // checking.
    return 0;
  }

  // We interpret data as a small DSL for exploring the state space of
  // HugePageAwareAllocator.
  //
  // [0] - Memory tag.
  // [1] - HugeRegionsMode.
  // [2] - Lifetime allocator options: Mode.
  // [3] - Lifetime allocator options: Strategy.
  // [4] - Lifetime allocator options: Short-lived threshold.
  // [12:5] - Reserved.
  //
  // Afterwards, we read 9 bytes at a time until the buffer is exhausted.
  // [i + 0]        - Specifies an operation to perform on the allocator
  // [i + 1, i + 8] - Specifies an integer. We use this as a source of
  //                  deterministic entropy to allow inputs to be replayed.
  //                  For example, this input can provide a Length to
  //                  allocate, or the index of the previous allocation to
  //                  deallocate.

  constexpr MemoryTag kTagOptions[] = {
      MemoryTag::kSampled, MemoryTag::kNormalP0, MemoryTag::kNormalP1,
      MemoryTag::kNormal, MemoryTag::kCold};
  constexpr int kTagSize = sizeof(kTagOptions) / sizeof(MemoryTag);
  static_assert(kTagSize > 0);
  MemoryTag tag = kTagOptions[data[0] % kTagSize];
  // Use kNormalP1 memory tag only if we have more than one NUMA partitions.
  tag = (kNumaPartitions == 1 && tag == MemoryTag::kNormalP1)
            ? MemoryTag::kNormalP0
            : tag;

  HugeRegionCountOption huge_region_option =
      data[1] >= 128 ? HugeRegionCountOption::kSlack
                     : HugeRegionCountOption::kAbandonedCount;

  // Initialize lifetime-aware allocator.
  LifetimePredictionOptions::Mode mode =
      data[2] < 85
          ? LifetimePredictionOptions::Mode::kEnabled
          : (data[2] < 170 ? LifetimePredictionOptions::Mode::kDisabled
                           : LifetimePredictionOptions::Mode::kCounterfactual);

  LifetimePredictionOptions::Strategy strategy =
      data[3] >= 128
          ? LifetimePredictionOptions::Strategy::kAlwaysShortLivedRegions
          : LifetimePredictionOptions::Strategy::kPredictedLifetimeRegions;
  absl::Duration lifetime_duration = absl::Milliseconds(data[4]);
  LifetimePredictionOptions lifetime_options(mode, strategy, lifetime_duration);

  // data[12:5] - Reserve eight additional bytes for any features we might want
  // to add in the future.
  data += 13;
  size -= 13;

  // HugePageAwareAllocator can't be destroyed cleanly, so we store a pointer
  // to one and construct in place.
  void* p = malloc(sizeof(HugePageAwareAllocator));
  HugePageAwareAllocator* allocator;
  allocator =
      new (p) HugePageAwareAllocator(tag, huge_region_option, lifetime_options);

  struct SpanInfo {
    Span* span;
    size_t objects_per_span;
  };
  std::vector<SpanInfo> allocs;
  Length allocated;

  // TODO(b/271282540): Add an additional op that simulates a failure for
  // Unback when releasing spans.
  for (size_t i = 0; i + 9 <= size; i += 9) {
    const uint16_t op = data[i];
    uint64_t value;
    memcpy(&value, &data[i + 1], sizeof(value));

    switch (op & 0x7) {
      case 0: {
        // Aligned allocate.  We divide up our random value by:
        //
        // value[0:15]  - We choose a Length to allocate.
        // value[16:31] - We select num_to_objects, i.e. the number of objects
        // to allocate.
        // value[32:47] - Alignment.
        // value[48] - Should we use aligned allocate?
        // value[63:49] - Reserved.
        const Length length(std::clamp<size_t>(
            value & 0xFFFF, 1, kPagesPerHugePage.raw_num() - 1));
        const size_t num_objects = (value >> 16) & 0xFFFF;

        const bool use_aligned = ((value >> 48) & 0x1) == 0;
        Span* s;
        if (use_aligned) {
          const Length align(std::clamp<size_t>(
              (value >> 32) & 0xFFFF, 1, kPagesPerHugePage.raw_num() - 1));
          s = allocator->NewAligned(length, align, num_objects);
        } else {
          s = allocator->New(length, num_objects);
        }
        CHECK_CONDITION(s != nullptr);
        CHECK_GE(s->num_pages().raw_num(), length.raw_num());

        allocs.push_back(SpanInfo{s, num_objects});
        allocated += s->num_pages();
        break;
      }
      case 1: {
        // Deallocate.  We divide up our random value by:
        //
        // value - We choose index in allocs to deallocate a span.

        if (allocs.empty()) break;

        const size_t pos = value % allocs.size();
        std::swap(allocs[pos], allocs[allocs.size() - 1]);

        SpanInfo span_info = allocs[allocs.size() - 1];
        allocs.resize(allocs.size() - 1);
        allocated -= span_info.span->num_pages();
        {
          absl::base_internal::SpinLockHolder h(&pageheap_lock);
          allocator->Delete(span_info.span, span_info.objects_per_span);
        }
        break;
      }
      case 2: {
        // Release pages.  We divide up our random value by:
        //
        // value[7:0] - Choose number of pages to release.
        // value[63:8] - Reserved.
        Length desired(value & 0x00FF);
        {
          absl::base_internal::SpinLockHolder h(&pageheap_lock);
          allocator->ReleaseAtLeastNPages(desired);
        }
        break;
      }
      case 3: {
        // Release pages by breaking hugepages.  We divide up our random value
        // by:
        //
        // value[7:0] - Choose number of pages to release.
        // value[63:8] - Reserved.
        Length desired(value & 0x00FF);
        Length released;
        BackingStats stats;
        {
          absl::base_internal::SpinLockHolder h(&pageheap_lock);
          stats = allocator->stats();
          released = allocator->ReleaseAtLeastNPagesBreakingHugepages(desired);
        }
        CHECK_GE(released.in_bytes(),
                 std::min(desired.in_bytes(), stats.free_bytes));
        break;
      }
      case 4: {
        // Gather stats in pbtxt format.
        //
        // value is unused.
        std::string s;
        s.resize(1 << 20);
        Printer p(&s[0], s.size());
        PbtxtRegion region(&p, kTop);
        allocator->PrintInPbtxt(&region);
        break;
      }
      case 5: {
        // Print stats.
        //
        // value[0]: Choose if we print everything.
        // value[63:1]: Reserved.
        std::string s;
        s.resize(1 << 20);
        Printer p(&s[0], s.size());
        bool everything = (value % 2 == 0);
        allocator->Print(&p, everything);
        break;
      }
      case 6: {
        // Gather and check stats.
        //
        // value is unused.
        BackingStats stats;
        {
          absl::base_internal::SpinLockHolder h(&pageheap_lock);
          stats = allocator->stats();
        }
        uint64_t used_bytes =
            stats.system_bytes - stats.free_bytes - stats.unmapped_bytes;
        CHECK_EQ(used_bytes, allocated.in_bytes());
        break;
      }
    }
  }

  // Clean up.
  for (auto span_info : allocs) {
    absl::base_internal::SpinLockHolder h(&pageheap_lock);
    allocated -= span_info.span->num_pages();
    allocator->Delete(span_info.span, span_info.objects_per_span);
  }
  CHECK_EQ(allocated.in_bytes(), 0);
  free(allocator);
  return 0;
}