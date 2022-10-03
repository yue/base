// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef BASE_PROFILER_LIBUNWINDSTACK_UNWINDER_ANDROID_H_
#define BASE_PROFILER_LIBUNWINDSTACK_UNWINDER_ANDROID_H_

#include <memory>
#include <vector>

#include "base/profiler/unwinder.h"
#include "base/time/time.h"
#include "third_party/libunwindstack/src/libunwindstack/include/unwindstack/Maps.h"
#include "third_party/libunwindstack/src/libunwindstack/include/unwindstack/Memory.h"

namespace base {
// This unwinder uses the libunwindstack::Unwinder internally to provide the
// base::Unwinder implementation. This is in contrast to
// base::NativeUnwinderAndroid, which uses functions from libunwindstack
// selectively to provide a subset of libunwindstack::Unwinder features. This
// causes some divergences from other base::Unwinder (this unwinder either fully
// succeeds or fully fails). A good source for a compariative unwinder would be
// traced_perf or heapprofd on android which uses the same API.
class LibunwindstackUnwinderAndroid
    : public Unwinder,
      public ModuleCache::AuxiliaryModuleProvider {
 public:
  LibunwindstackUnwinderAndroid();
  ~LibunwindstackUnwinderAndroid() override;

  LibunwindstackUnwinderAndroid(const LibunwindstackUnwinderAndroid&) = delete;
  LibunwindstackUnwinderAndroid& operator=(
      const LibunwindstackUnwinderAndroid&) = delete;

  // Unwinder
  void InitializeModules() override;
  bool CanUnwindFrom(const Frame& current_frame) const override;
  UnwindResult TryUnwind(RegisterContext* thread_context,
                         uintptr_t stack_top,
                         std::vector<Frame>* stack) override;

  // ModuleCache::AuxiliaryModuleProvider
  std::unique_ptr<const ModuleCache::Module> TryCreateModuleForAddress(
      uintptr_t address) override;

 private:
  int samples_since_last_maps_parse_ = 0;
  std::unique_ptr<unwindstack::Maps> memory_regions_map_;
  // libunwindstack::Unwinder requires that process_memory be provided as a
  // std::shared_ptr. Since this is a third_party library this exception to
  // normal Chromium conventions of not using shared_ptr has to exist here.
  std::shared_ptr<unwindstack::Memory> process_memory_;
};
}  // namespace base

#endif  // BASE_PROFILER_LIBUNWINDSTACK_UNWINDER_ANDROID_H_