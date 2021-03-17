// Copyright (c) 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/allocator/partition_allocator/pcscan.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <mutex>
#include <numeric>
#include <set>
#include <thread>
#include <vector>

#include "base/allocator/partition_allocator/address_pool_manager.h"
#include "base/allocator/partition_allocator/address_pool_manager_bitmap.h"
#include "base/allocator/partition_allocator/object_bitmap.h"
#include "base/allocator/partition_allocator/page_allocator.h"
#include "base/allocator/partition_allocator/page_allocator_constants.h"
#include "base/allocator/partition_allocator/partition_address_space.h"
#include "base/allocator/partition_allocator/partition_alloc.h"
#include "base/allocator/partition_allocator/partition_alloc_check.h"
#include "base/allocator/partition_allocator/partition_alloc_constants.h"
#include "base/allocator/partition_allocator/partition_alloc_features.h"
#include "base/allocator/partition_allocator/partition_page.h"
#include "base/compiler_specific.h"
#include "base/cpu.h"
#include "base/debug/alias.h"
#include "base/immediate_crash.h"
#include "base/logging.h"
#include "base/metrics/histogram_macros.h"
#include "base/no_destructor.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/trace_event/base_tracing.h"
#include "build/build_config.h"

#if defined(ARCH_CPU_X86_64)
// Include order is important, so we disable formatting.
// clang-format off
#include <immintrin.h>
// Including these headers directly should generally be avoided. For the
// scanning loop, we check at runtime which SIMD extension we can use. Since
// Chrome is compiled with -msse3 (the minimal requirement), we include the
// headers directly to make the intrinsics available. Another option could be to
// use inline assembly, but that would hinder compiler optimization for
// vectorized instructions.
#include <avxintrin.h>
#include <avx2intrin.h>
// clang-format on
#endif

namespace base {
namespace internal {

[[noreturn]] BASE_EXPORT NOINLINE NOT_TAIL_CALLED void DoubleFreeAttempt() {
  NO_CODE_FOLDING();
  IMMEDIATE_CRASH();
}

namespace {

#if defined(PA_HAS_64_BITS_POINTERS)
// Bytemap that represent regions (cards) that contain quarantined objects.
// A single PCScan cycle consists of the following steps:
// 1) clearing (memset quarantine + marking cards that contain quarantine);
// 2) scanning;
// 3) sweeping (freeing + unmarking cards that contain freed objects).
// Marking cards on step 1) ensures that the card table stays in the consistent
// state while scanning. Unmarking on the step 3) ensures that unmarking
// actually happens (and we don't hit too many false positives).
class QuarantineCardTable final {
 public:
  // Avoid the load of the base of the normal bucket pool.
  ALWAYS_INLINE static QuarantineCardTable& GetFrom(uintptr_t ptr) {
    constexpr uintptr_t kNormalBucketPoolMask =
        PartitionAddressSpace::NormalBucketPoolBaseMask();
    return *reinterpret_cast<QuarantineCardTable*>(ptr & kNormalBucketPoolMask);
  }

  ALWAYS_INLINE void Quarantine(uintptr_t begin, size_t size) {
    return SetImpl(begin, size, true);
  }

  ALWAYS_INLINE void Unquarantine(uintptr_t begin, size_t size) {
    return SetImpl(begin, size, false);
  }

  // Returns whether the card to which |ptr| points to contains quarantined
  // objects. May return false positives for but should never return false
  // negatives, as otherwise this breaks security.
  ALWAYS_INLINE bool IsQuarantined(uintptr_t ptr) const {
    const size_t byte = Byte(ptr);
    PA_DCHECK(byte < bytes_.size());
    return bytes_[byte];
  }

 private:
  static constexpr size_t kCardSize =
      AddressPoolManager::kNormalBucketMaxSize / kSuperPageSize;
  static constexpr size_t kBytes =
      AddressPoolManager::kNormalBucketMaxSize / kCardSize;

  QuarantineCardTable() = default;

  ALWAYS_INLINE static constexpr size_t Byte(uintptr_t address) {
    constexpr uintptr_t kNormalBucketPoolMask =
        PartitionAddressSpace::NormalBucketPoolBaseMask();
    return (address & ~kNormalBucketPoolMask) / kCardSize;
  }

  ALWAYS_INLINE void SetImpl(uintptr_t begin, size_t size, bool value) {
    const size_t byte = Byte(begin);
    const size_t need_bytes = (size + (kCardSize - 1)) / kCardSize;
    PA_DCHECK(bytes_.size() >= byte + need_bytes);
    PA_DCHECK(PartitionAddressSpace::IsInNormalBucketPool(
        reinterpret_cast<void*>(begin)));
    for (size_t i = byte; i < byte + need_bytes; ++i)
      bytes_[i] = value;
  }

  std::array<bool, kBytes> bytes_;
};
static_assert(kSuperPageSize >= sizeof(QuarantineCardTable),
              "Card table size must be less than kSuperPageSize, since this is "
              "what is committed");
#endif

ThreadSafePartitionRoot& PCScanMetadataAllocator() {
  static base::NoDestructor<ThreadSafePartitionRoot> allocator{
      PartitionOptions{PartitionOptions::Alignment::kRegular,
                       PartitionOptions::ThreadCache::kDisabled,
                       PartitionOptions::Quarantine::kDisallowed,
                       PartitionOptions::RefCount::kDisabled}};
  return *allocator;
}

// STL allocator which is needed to keep internal data structures required by
// PCScan.
template <typename T>
class MetadataAllocator {
 public:
  using value_type = T;

  MetadataAllocator() = default;

  template <typename U>
  MetadataAllocator(const MetadataAllocator<U>&) {}  // NOLINT

  template <typename U>
  MetadataAllocator& operator=(const MetadataAllocator<U>&) {}

  template <typename U>
  bool operator==(const MetadataAllocator<U>&) {
    return true;
  }

  value_type* allocate(size_t size) {
    return static_cast<value_type*>(PCScanMetadataAllocator().AllocFlagsNoHooks(
        0, size * sizeof(value_type)));
  }

  void deallocate(value_type* ptr, size_t size) {
    PCScanMetadataAllocator().FreeNoHooks(ptr);
  }
};

void LogStats(size_t swept_bytes, size_t last_size, size_t new_size) {
  VLOG(2) << "quarantine size: " << last_size << " -> " << new_size
          << ", swept bytes: " << swept_bytes
          << ", survival rate: " << static_cast<double>(new_size) / last_size;
}

ALWAYS_INLINE uintptr_t GetObjectStartInSuperPage(uintptr_t maybe_ptr,
                                                  const PCScan::Root& root) {
  char* allocation_start =
      GetSlotStartInSuperPage<ThreadSafe>(reinterpret_cast<char*>(maybe_ptr));
  if (!allocation_start) {
    // |maybe_ptr| refers to a garbage or is outside of the payload region.
    return 0;
  }
  return reinterpret_cast<uintptr_t>(
      root.AdjustPointerForExtrasAdd(allocation_start));
}

#if DCHECK_IS_ON()
bool IsScannerQuarantineBitmapEmpty(char* super_page, size_t epoch) {
  auto* bitmap = QuarantineBitmapFromPointer(QuarantineBitmapType::kScanner,
                                             epoch, super_page);
  size_t visited = 0;
  bitmap->Iterate([&visited](auto) { ++visited; });
  return !visited;
}
#endif

#define FOR_ALL_PCSCAN_SCANNER_SCOPES(V) \
  V(Clear)                               \
  V(Scan)                                \
  V(Sweep)                               \
  V(Overall)

#define FOR_ALL_PCSCAN_MUTATOR_SCOPES(V) \
  V(Clear)                               \
  V(Scan)                                \
  V(Overall)

class StatsCollector final {
 public:
  enum class ScannerId {
#define DECLARE_ENUM(name) k##name,
    FOR_ALL_PCSCAN_SCANNER_SCOPES(DECLARE_ENUM)
#undef DECLARE_ENUM
        kNumIds,
  };

  enum class MutatorId {
#define DECLARE_ENUM(name) k##name,
    FOR_ALL_PCSCAN_MUTATOR_SCOPES(DECLARE_ENUM)
#undef DECLARE_ENUM
        kNumIds,
  };

  enum class Context {
    // For tasks executed from mutator threads (safepoints).
    kMutator,
    // For concurrent scanner tasks.
    kScanner
  };

  template <Context context>
  class Scope final {
    using IdType =
        std::conditional_t<context == Context::kMutator, MutatorId, ScannerId>;

   public:
    Scope(StatsCollector& stats, IdType type)
        : stats_(stats), type_(type), start_time_(base::TimeTicks::Now()) {
      TRACE_EVENT_BEGIN0(kTraceCategory, ToTracingString(type));
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    ~Scope() {
      TRACE_EVENT_END0(kTraceCategory, ToTracingString(type_));
      stats_.IncreaseScopeTime(type_, base::TimeTicks::Now() - start_time_);
    }

   private:
    static constexpr char kTraceCategory[] = "partition_alloc";

    static constexpr const char* ToTracingString(ScannerId id) {
      switch (id) {
        case ScannerId::kClear:
          return "PCScan.Scanner.Clear";
        case ScannerId::kScan:
          return "PCScan.Scanner.Scan";
        case ScannerId::kSweep:
          return "PCScan.Scanner.Sweep";
        case ScannerId::kOverall:
          return "PCScan.Scanner";
        case ScannerId::kNumIds:
          __builtin_unreachable();
      }
    }

    static constexpr const char* ToTracingString(MutatorId id) {
      switch (id) {
        case MutatorId::kClear:
          return "PCScan.Mutator.Clear";
        case MutatorId::kScan:
          return "PCScan.Mutator.Scan";
        case MutatorId::kOverall:
          return "PCScan.Mutator";
        case MutatorId::kNumIds:
          __builtin_unreachable();
      }
    }

    StatsCollector& stats_;
    IdType type_;
    base::TimeTicks start_time_;
  };

  using ScannerScope = Scope<Context::kScanner>;
  using MutatorScope = Scope<Context::kMutator>;

  explicit StatsCollector(const char* process_name)
      : process_name_(process_name) {}

  StatsCollector(const StatsCollector&) = delete;
  StatsCollector& operator=(const StatsCollector&) = delete;

  void IncreaseScopeTime(ScannerId type, base::TimeDelta duration) {
    const int64_t ms = duration.InMicroseconds();
    PA_DCHECK(ms <= std::numeric_limits<uint32_t>::max());
    scanner_scopes_[static_cast<size_t>(type)].fetch_add(
        ms, std::memory_order_relaxed);
  }

  void IncreaseScopeTime(MutatorId type, base::TimeDelta duration) {
    const int64_t ms = duration.InMicroseconds();
    PA_DCHECK(ms <= std::numeric_limits<uint32_t>::max());
    mutator_scopes_[static_cast<size_t>(type)].fetch_add(
        ms, std::memory_order_relaxed);
  }

  void IncreaseSurvivedQuarantineSize(size_t size) {
    survived_quarantine_size_.fetch_add(size, std::memory_order_relaxed);
  }
  size_t survived_quarantine_size() const {
    return survived_quarantine_size_.load(std::memory_order_relaxed);
  }

  void IncreaseSweptSize(size_t size) { swept_size_ += size; }
  size_t swept_size() const { return swept_size_; }

  void UpdateHistograms() {
    if (!process_name_) {
      // Don't update histograms if |process_name_| is not set.
      return;
    }
#define UPDATE_UMA(name)                                                 \
  UMA_HISTOGRAM_TIMES(                                                   \
      ToUMAString(ScannerId::k##name).c_str(),                           \
      base::TimeDelta::FromMicroseconds(                                 \
          scanner_scopes_[static_cast<size_t>(ScannerId::k##name)].load( \
              std::memory_order_relaxed)));
    FOR_ALL_PCSCAN_SCANNER_SCOPES(UPDATE_UMA)
#undef UPDATE_UMA
#define UPDATE_UMA(name)                                                 \
  UMA_HISTOGRAM_TIMES(                                                   \
      ToUMAString(MutatorId::k##name).c_str(),                           \
      base::TimeDelta::FromMicroseconds(                                 \
          mutator_scopes_[static_cast<size_t>(MutatorId::k##name)].load( \
              std::memory_order_relaxed)));
    FOR_ALL_PCSCAN_MUTATOR_SCOPES(UPDATE_UMA)
#undef UPDATE_UMA
  }

 private:
  using MetadataString =
      std::basic_string<char, std::char_traits<char>, MetadataAllocator<char>>;

  MetadataString ToUMAString(ScannerId id) const {
    PA_DCHECK(process_name_);
    const MetadataString process_name = process_name_;
    switch (id) {
      case ScannerId::kClear:
        return "PA.PCScan." + process_name + ".Scanner.Clear";
      case ScannerId::kScan:
        return "PA.PCScan." + process_name + ".Scanner.Scan";
      case ScannerId::kSweep:
        return "PA.PCScan." + process_name + ".Scanner.Sweep";
      case ScannerId::kOverall:
        return "PA.PCScan." + process_name + ".Scanner";
      case ScannerId::kNumIds:
        __builtin_unreachable();
    }
  }

  MetadataString ToUMAString(MutatorId id) const {
    PA_DCHECK(process_name_);
    const MetadataString process_name = process_name_;
    switch (id) {
      case MutatorId::kClear:
        return "PA.PCScan." + process_name + ".Mutator.Clear";
      case MutatorId::kScan:
        return "PA.PCScan." + process_name + ".Mutator.Scan";
      case MutatorId::kOverall:
        return "PA.PCScan." + process_name + ".Mutator";
      case MutatorId::kNumIds:
        __builtin_unreachable();
    }
  }

  std::array<std::atomic<uint32_t>, static_cast<size_t>(ScannerId::kNumIds)>
      scanner_scopes_;
  std::array<std::atomic<uint32_t>, static_cast<size_t>(MutatorId::kNumIds)>
      mutator_scopes_;
  std::atomic<size_t> survived_quarantine_size_{0u};
  size_t swept_size_ = 0u;
  const char* process_name_ = nullptr;
};

#undef FOR_ALL_PCSCAN_MUTATOR_SCOPES
#undef FOR_ALL_PCSCAN_SCANNER_SCOPES

enum class SimdSupport : uint8_t {
  kUnvectorized,
  kSSE3,
  kAVX2,
  // TODO(bikineev): Add support for Neon.
};

SimdSupport DetectSimdSupport() {
  base::CPU cpu;
  if (cpu.has_avx2())
    return SimdSupport::kAVX2;
  if (cpu.has_sse3())
    return SimdSupport::kSSE3;
  return SimdSupport::kUnvectorized;
}

// Internal PCScan singleton. The separation between frontend and backend is
// needed to keep access to the hot data (quarantine) in the frontend fast,
// whereas the backend can hold cold data.
class PCScanInternal final {
 public:
  using Root = PCScan::Root;

  static constexpr size_t kMaxNumberOfRoots = 8u;
  class Roots final : private std::array<Root*, kMaxNumberOfRoots> {
    using Base = std::array<Root*, kMaxNumberOfRoots>;

   public:
    using typename Base::const_iterator;
    using typename Base::iterator;

    // Explicitly value-initialize Base{} as otherwise the default
    // (aggregate) initialization won't be considered as constexpr.
    constexpr Roots() : Base{} {}

    iterator begin() { return Base::begin(); }
    const_iterator begin() const { return Base::begin(); }

    iterator end() { return begin() + current_; }
    const_iterator end() const { return begin() + current_; }

    void Add(Root* root);

    size_t size() const { return current_; }

    void ClearForTesting();  // IN-TEST

   private:
    size_t current_ = 0u;
  };

  static PCScanInternal& Instance() {
    // Since the data that PCScanInternal holds is cold, it's fine to have the
    // runtime check for thread-safe local static initialization.
    static base::NoDestructor<PCScanInternal> instance;
    return *instance;
  }

  PCScanInternal(const PCScanInternal&) = delete;
  PCScanInternal& operator=(const PCScanInternal&) = delete;

  void RegisterScannableRoot(Root* root);
  void RegisterNonScannableRoot(Root* root);

  Roots& scannable_roots() { return scannable_roots_; }
  const Roots& scannable_roots() const { return scannable_roots_; }

  Roots& nonscannable_roots() { return nonscannable_roots_; }
  const Roots& nonscannable_roots() const { return nonscannable_roots_; }

  void SetProcessName(const char* name);
  const char* process_name() const { return process_name_; }

  // Get size of all committed pages from scannable and nonscannable roots.
  size_t CalculateTotalHeapSize() const;

  SimdSupport simd_support() const { return simd_support_; }

  void ClearRootsForTesting();  // IN-TEST

 private:
  friend base::NoDestructor<PCScanInternal>;

  PCScanInternal();

  Roots scannable_roots_{};
  Roots nonscannable_roots_{};
  const char* process_name_ = nullptr;
  const SimdSupport simd_support_;
};

void PCScanInternal::Roots::Add(Root* root) {
  PA_CHECK(std::find(begin(), end(), root) == end());
  (*this)[current_] = root;
  ++current_;
  PA_CHECK(current_ != kMaxNumberOfRoots)
      << "Exceeded number of allowed partition roots";
}

void PCScanInternal::Roots::ClearForTesting() {
  std::fill(begin(), end(), nullptr);
  current_ = 0;
}

PCScanInternal::PCScanInternal() : simd_support_(DetectSimdSupport()) {
#if defined(PA_HAS_64_BITS_POINTERS)
  if (features::IsPartitionAllocGigaCageEnabled()) {
    PartitionAddressSpace::Init();
    RecommitSystemPages(
        reinterpret_cast<void*>(PartitionAddressSpace::NormalBucketPoolBase()),
        sizeof(QuarantineCardTable), PageReadWrite, PageUpdatePermissions);
  }
#endif
}

void CommitQuarantineBitmaps(PCScan::Root& root) {
  size_t quarantine_bitmaps_size_to_commit = CommittedQuarantineBitmapsSize();
  for (auto* super_page_extent = root.first_extent; super_page_extent;
       super_page_extent = super_page_extent->next) {
    for (char* super_page = super_page_extent->super_page_base;
         super_page != super_page_extent->super_pages_end;
         super_page += kSuperPageSize) {
      RecommitSystemPages(internal::SuperPageQuarantineBitmaps(super_page),
                          quarantine_bitmaps_size_to_commit, PageReadWrite,
                          PageUpdatePermissions);
    }
  }
}

void PCScanInternal::RegisterScannableRoot(Root* root) {
  PA_DCHECK(root);
  PA_CHECK(root->IsQuarantineAllowed());
  typename Root::ScopedGuard guard(root->lock_);
  if (root->IsScanEnabled())
    return;
  PA_CHECK(!root->IsQuarantineEnabled());
  CommitQuarantineBitmaps(*root);
  root->scan_mode = Root::ScanMode::kEnabled;
  root->quarantine_mode = Root::QuarantineMode::kEnabled;
  scannable_roots_.Add(root);
}

void PCScanInternal::RegisterNonScannableRoot(Root* root) {
  PA_DCHECK(root);
  PA_CHECK(root->IsQuarantineAllowed());
  typename Root::ScopedGuard guard(root->lock_);
  if (root->IsQuarantineEnabled())
    return;
  CommitQuarantineBitmaps(*root);
  root->quarantine_mode = Root::QuarantineMode::kEnabled;
  nonscannable_roots_.Add(root);
}

void PCScanInternal::SetProcessName(const char* process_name) {
  PA_DCHECK(process_name);
  PA_DCHECK(!process_name_);
  process_name_ = process_name;
}

size_t PCScanInternal::CalculateTotalHeapSize() const {
  const auto acc = [](size_t size, Root* root) {
    return size + root->get_total_size_of_committed_pages();
  };
  return std::accumulate(scannable_roots_.begin(), scannable_roots_.end(), 0u,
                         acc) +
         std::accumulate(nonscannable_roots_.begin(), nonscannable_roots_.end(),
                         0u, acc);
}

void PCScanInternal::ClearRootsForTesting() {
  scannable_roots_.ClearForTesting();     // IN-TEST
  nonscannable_roots_.ClearForTesting();  // IN-TEST
}

class PCScanSnapshot final {
 public:
  struct ScanArea {
    ScanArea(uintptr_t* begin, uintptr_t* end) : begin(begin), end(end) {}

    uintptr_t* begin = nullptr;
    uintptr_t* end = nullptr;
  };
  using ScanAreas = std::vector<ScanArea, MetadataAllocator<ScanArea>>;

  // Large scan areas have their slot size recorded which allows to iterate
  // based on objects, potentially skipping over objects if possible.
  struct LargeScanArea : public ScanArea {
    LargeScanArea(uintptr_t* begin, uintptr_t* end, size_t slot_size)
        : ScanArea(begin, end), slot_size(slot_size) {}

    size_t slot_size = 0;
  };
  using LargeScanAreas =
      std::vector<LargeScanArea, MetadataAllocator<LargeScanArea>>;

  // Super pages only correspond to normal buckets.
  using SuperPages =
      std::set<uintptr_t, std::less<>, MetadataAllocator<uintptr_t>>;

  PCScanSnapshot() = default;

  void Take(size_t pcscan_epoch);

  ScanAreas& scan_areas() { return scan_areas_; }
  const ScanAreas& scan_areas() const { return scan_areas_; }

  LargeScanAreas& large_scan_areas() { return large_scan_areas_; }
  const LargeScanAreas& large_scan_areas() const { return large_scan_areas_; }

  SuperPages& quarantinable_super_pages() { return super_pages_; }
  const SuperPages& quarantinable_super_pages() const { return super_pages_; }

 private:
  ScanAreas scan_areas_;
  LargeScanAreas large_scan_areas_;
  SuperPages super_pages_;
};

void PCScanSnapshot::Take(size_t pcscan_epoch) {
  using Root = PartitionRoot<ThreadSafe>;
  using SlotSpan = SlotSpanMetadata<ThreadSafe>;
  // Threshold for which bucket size it is worthwhile in checking whether the
  // object is a quarantined object and can be skipped.
  static constexpr size_t kLargeScanAreaThreshold = 8192;
  // Take a snapshot of all allocated non-empty slot spans.
  static constexpr size_t kScanAreasReservationSize = 128;

  scan_areas_.reserve(kScanAreasReservationSize);

  auto& pcscan_internal = PCScanInternal::Instance();
  for (Root* root : pcscan_internal.scannable_roots()) {
    typename Root::ScopedGuard guard(root->lock_);

    // Take a snapshot of all super pages and scannable slot spans.
    // TODO(bikineev): Consider making current_extent lock-free and moving it
    // to the concurrent thread.
    for (auto* super_page_extent = root->first_extent; super_page_extent;
         super_page_extent = super_page_extent->next) {
      for (char* super_page = super_page_extent->super_page_base;
           super_page != super_page_extent->super_pages_end;
           super_page += kSuperPageSize) {
        // TODO(bikineev): Consider following freelists instead of slot spans.
        const size_t visited_slot_spans = IterateSlotSpans<ThreadSafe>(
            super_page, true /*with_quarantine*/,
            [this](SlotSpan* slot_span) -> bool {
              if (slot_span->is_empty() || slot_span->is_decommitted()) {
                return false;
              }
              auto* payload_begin = static_cast<uintptr_t*>(
                  SlotSpan::ToSlotSpanStartPtr(slot_span));
              size_t provisioned_size = slot_span->GetProvisionedSize();
              // Free & decommitted slot spans are skipped.
              PA_DCHECK(provisioned_size > 0);
              auto* payload_end =
                  payload_begin + (provisioned_size / sizeof(uintptr_t));
              if (slot_span->bucket->slot_size >= kLargeScanAreaThreshold) {
                large_scan_areas_.push_back(
                    {payload_begin, payload_end, slot_span->bucket->slot_size});
              } else {
                scan_areas_.push_back({payload_begin, payload_end});
              }
              return true;
            });
        // If we haven't visited any slot spans, all the slot spans in the
        // super-page are either empty or decommitted. This means that all the
        // objects are freed and there are no quarantined objects.
        if (LIKELY(visited_slot_spans)) {
          super_pages_.insert(reinterpret_cast<uintptr_t>(super_page));
        } else {
#if DCHECK_IS_ON()
          PA_CHECK(IsScannerQuarantineBitmapEmpty(super_page, pcscan_epoch));
#endif
        }
      }
    }
  }
  for (Root* root : pcscan_internal.nonscannable_roots()) {
    typename Root::ScopedGuard guard(root->lock_);
    // Take a snapshot of all super pages and nnonscannable slot spans.
    for (auto* super_page_extent = root->first_extent; super_page_extent;
         super_page_extent = super_page_extent->next) {
      for (char* super_page = super_page_extent->super_page_base;
           super_page != super_page_extent->super_pages_end;
           super_page += kSuperPageSize) {
        super_pages_.insert(reinterpret_cast<uintptr_t>(super_page));
      }
    }
  }
}

}  // namespace

// This class is responsible for performing the entire PCScan task.
// TODO(bikineev): Move PCScan algorithm out of PCScanTask.
class PCScanTask final {
 public:
  static void* operator new(size_t size) {
    return PCScanMetadataAllocator().AllocFlagsNoHooks(0, size);
  }

  static void operator delete(void* ptr) {
    PCScanMetadataAllocator().FreeNoHooks(ptr);
  }

  // Creates and initializes a PCScan state.
  explicit PCScanTask(PCScan& pcscan);

  PCScanTask(PCScanTask&&) noexcept = delete;
  PCScanTask& operator=(PCScanTask&&) noexcept = delete;

  // Execute PCScan from the scanner thread. Must be called only once from the
  // scanner thread.
  void RunFromScanner();

 private:
  class ScanLoop;

  using Root = PCScan::Root;
  using SlotSpan = SlotSpanMetadata<ThreadSafe>;

  struct GigaCageLookupPolicy {
    ALWAYS_INLINE bool TestOnHeapPointer(uintptr_t maybe_ptr) const {
#if defined(PA_HAS_64_BITS_POINTERS)
#if DCHECK_IS_ON()
      PA_DCHECK(IsManagedByPartitionAllocNormalBuckets(
          reinterpret_cast<void*>(maybe_ptr)));
#endif
      return QuarantineCardTable::GetFrom(maybe_ptr).IsQuarantined(maybe_ptr);
#else
      return IsManagedByPartitionAllocNormalBuckets(
          reinterpret_cast<void*>(maybe_ptr));
#endif  // PA_HAS_64_BITS_POINTERS
    }
    [[maybe_unused]] const PCScanSnapshot& snapshot;
  };

  struct NoGigaCageLookupPolicy {
    ALWAYS_INLINE bool TestOnHeapPointer(uintptr_t maybe_ptr) const {
      const auto super_page_base = maybe_ptr & kSuperPageBaseMask;
      const auto& super_pages = snapshot.quarantinable_super_pages();
      auto it = super_pages.lower_bound(super_page_base);
      return it != super_pages.end() && *it == super_page_base;
    }
    const PCScanSnapshot& snapshot;
  };

  template <typename LookupPolicy>
  ALWAYS_INLINE QuarantineBitmap* TryFindScannerBitmapForPointer(
      uintptr_t maybe_ptr) const;

  // Lookup and marking functions. Return size of the object if marked or zero
  // otherwise.
  template <typename LookupPolicy>
  ALWAYS_INLINE size_t
  TryMarkObjectInNormalBucketPool(uintptr_t maybe_ptr) const;

  // Scans all registeres partitions and marks reachable quarantined objects.
  // Returns the size of marked objects.
  void ScanPartitions();

  // Clear quarantined objects and prepare card table for fast lookup
  void ClearQuarantinedObjectsAndPrepareCardTable();

  // Sweeps (frees) unreachable quarantined entries. Returns the size of swept
  // objects.
  void SweepQuarantine();

  // Finishes the scanner (updates limits, UMA, etc).
  void FinishScanner();

  // Cache the pcscan epoch to avoid the compiler loading the atomic
  // QuarantineData::epoch_ on each access.
  const size_t pcscan_epoch_;
  PCScanSnapshot snapshot_;
  StatsCollector stats_;
  PCScan& pcscan_;
};

template <typename LookupPolicy>
ALWAYS_INLINE QuarantineBitmap* PCScanTask::TryFindScannerBitmapForPointer(
    uintptr_t maybe_ptr) const {
  // First, check if |maybe_ptr| points to a valid super page or a quarantined
  // card.
  LookupPolicy lookup{snapshot_};
  if (LIKELY(!lookup.TestOnHeapPointer(maybe_ptr)))
    return nullptr;
  // Check if we are not pointing to metadata/guard pages.
  if (!IsWithinSuperPagePayload(reinterpret_cast<char*>(maybe_ptr),
                                true /*with quarantine*/))
    return nullptr;
  // We are certain here that |maybe_ptr| points to the super page payload.
  return QuarantineBitmapFromPointer(QuarantineBitmapType::kScanner,
                                     pcscan_epoch_,
                                     reinterpret_cast<char*>(maybe_ptr));
}

// Looks up and marks a potential dangling pointer. Returns the size of the slot
// (which is then accounted as quarantined) or zero if no object is found.
// For normal bucket super pages, PCScan uses two quarantine bitmaps, the
// mutator and the scanner one. The former is used by mutators when objects are
// freed, while the latter is used concurrently by the PCScan thread. The
// bitmaps are swapped as soon as PCScan is triggered. Once a dangling pointer
// (which points to an object in the scanner bitmap) is found,
// TryMarkObjectInNormalBucketPool() marks it again in the bitmap and clears
// from the scanner bitmap. This way, when scanning is done, all uncleared
// entries in the scanner bitmap correspond to unreachable objects.
template <typename LookupPolicy>
ALWAYS_INLINE size_t
PCScanTask::TryMarkObjectInNormalBucketPool(uintptr_t maybe_ptr) const {
  using AccessType = QuarantineBitmap::AccessType;
  // Check if maybe_ptr points somewhere to the heap.
  auto* scanner_bitmap =
      TryFindScannerBitmapForPointer<LookupPolicy>(maybe_ptr);
  if (!scanner_bitmap)
    return 0;

  auto* root =
      Root::FromPointerInNormalBucketPool(reinterpret_cast<char*>(maybe_ptr));

  // Check if pointer was in the quarantine bitmap.
  const uintptr_t base = GetObjectStartInSuperPage(maybe_ptr, *root);
  if (!base || !scanner_bitmap->template CheckBit<AccessType::kNonAtomic>(base))
    return 0;

  PA_DCHECK((maybe_ptr & kSuperPageBaseMask) == (base & kSuperPageBaseMask));

  auto* target_slot_span =
      SlotSpan::FromSlotInnerPtr(reinterpret_cast<void*>(base));
  PA_DCHECK(root == Root::FromSlotSpan(target_slot_span));

  const size_t usable_size = target_slot_span->GetUsableSize(root);
  // Range check for inner pointers.
  if (maybe_ptr >= base + usable_size)
    return 0;

  // Now we are certain that |maybe_ptr| is a dangling pointer. Mark it again in
  // the mutator bitmap and clear from the scanner bitmap. Note that since
  // PCScan has exclusive access to the scanner bitmap, we can avoid atomic rmw
  // operation for it.
  scanner_bitmap->template ClearBit<AccessType::kNonAtomic>(base);
  QuarantineBitmapFromPointer(QuarantineBitmapType::kMutator, pcscan_epoch_,
                              reinterpret_cast<char*>(base))
      ->template SetBit<AccessType::kAtomic>(base);
  return target_slot_span->bucket->slot_size;
}

void PCScanTask::ClearQuarantinedObjectsAndPrepareCardTable() {
  using AccessType = QuarantineBitmap::AccessType;

  const bool giga_cage_enabled = features::IsPartitionAllocGigaCageEnabled();
  for (auto super_page : snapshot_.quarantinable_super_pages()) {
    auto* bitmap = QuarantineBitmapFromPointer(
        QuarantineBitmapType::kScanner, pcscan_epoch_,
        reinterpret_cast<char*>(super_page));
    auto* root = Root::FromSuperPage(reinterpret_cast<char*>(super_page));
    bitmap->template Iterate<AccessType::kNonAtomic>(
        [root, giga_cage_enabled](uintptr_t ptr) {
          auto* object = reinterpret_cast<void*>(ptr);
          auto* slot_span = SlotSpan::FromSlotInnerPtr(object);
          // Use zero as a zapping value to speed up the fast bailout check in
          // ScanPartitions.
          const size_t size = slot_span->GetUsableSize(root);
          memset(object, 0, size);
#if defined(PA_HAS_64_BITS_POINTERS)
          if (giga_cage_enabled) {
            // Set card(s) for this quarantined object.
            QuarantineCardTable::GetFrom(ptr).Quarantine(ptr, size);
          }
#else
          (void)giga_cage_enabled;
#endif
        });
  }
}

// Class used to perform actual scanning. Dispatches at runtime based on
// supported SIMD extensions.
class PCScanTask::ScanLoop final {
 public:
  explicit ScanLoop(const PCScanTask& pcscan_task)
      : scan_function_(GetScanFunction()),
        pcscan_task_(pcscan_task)
#if defined(PA_HAS_64_BITS_POINTERS)
        ,
        normal_bucket_pool_base_(PartitionAddressSpace::NormalBucketPoolBase())
#endif
  {
  }

  ScanLoop(const ScanLoop&) = delete;
  ScanLoop& operator=(const ScanLoop&) = delete;

  // Scans a range of addresses and marks reachable quarantined objects. Returns
  // the size of marked objects. The function race-fully reads the heap and
  // therefore TSAN is disabled for the dispatch functions.
  size_t Run(uintptr_t* begin, uintptr_t* end) const {
    static_assert(alignof(uintptr_t) % alignof(void*) == 0,
                  "Alignment of uintptr_t must be at least as strict as "
                  "alignment of a pointer type.");
    return (this->*scan_function_)(begin, end);
  }

 private:
  // This is to support polymorphic behavior and to avoid virtual calls.
  using ScanFunction = size_t (ScanLoop::*)(uintptr_t*, uintptr_t*) const;

  static ScanFunction GetScanFunction() {
    if (UNLIKELY(!features::IsPartitionAllocGigaCageEnabled())) {
      return &ScanLoop::RunUnvectorizedNoGigaCage;
    }
// We allow vectorization only for 64bit since they require support of the
// 64bit GigaCage, and only for x86 because a special instruction set is
// required.
#if defined(ARCH_CPU_X86_64)
    const SimdSupport simd = PCScanInternal::Instance().simd_support();
    if (simd == SimdSupport::kAVX2)
      return &ScanLoop::RunAVX2;
    if (simd == SimdSupport::kSSE3)
      return &ScanLoop::RunSSE3;
#endif
    return &ScanLoop::RunUnvectorized;
  }

#if defined(PA_HAS_64_BITS_POINTERS)
  ALWAYS_INLINE bool IsInNormalBucketPool(uintptr_t maybe_ptr) const {
    return (maybe_ptr & PartitionAddressSpace::NormalBucketPoolBaseMask()) ==
           normal_bucket_pool_base_;
  }
#endif

#if defined(ARCH_CPU_X86_64)
  __attribute__((target("sse3"))) NO_SANITIZE("thread") size_t
      RunSSE3(uintptr_t* begin, uintptr_t* end) const {
    static constexpr size_t kAlignmentRequirement = 16;
    static constexpr size_t kWordsInVector = 2;
    PA_DCHECK(!(reinterpret_cast<uintptr_t>(begin) % kAlignmentRequirement));
    PA_DCHECK(
        !((reinterpret_cast<char*>(end) - reinterpret_cast<char*>(begin)) %
          kAlignmentRequirement));
    // For SSE3, since some integer instructions are not yet available (e.g.
    // _mm_cmpeq_epi64), use packed doubles (not integers). Sticking to doubles
    // helps to avoid latency caused by "domain crossing penalties" (see bypass
    // delays in https://agner.org/optimize/microarchitecture.pdf).
    const __m128d vbase =
        _mm_castsi128_pd(_mm_set1_epi64x(normal_bucket_pool_base_));
    const __m128d cage_mask = _mm_castsi128_pd(
        _mm_set1_epi64x(PartitionAddressSpace::NormalBucketPoolBaseMask()));

    size_t quarantine_size = 0;
    for (uintptr_t* payload = begin; payload < end; payload += kWordsInVector) {
      const __m128d maybe_ptrs =
          _mm_load_pd(reinterpret_cast<double*>(payload));
      const __m128d vand = _mm_and_pd(maybe_ptrs, cage_mask);
      const __m128d vcmp = _mm_cmpeq_pd(vand, vbase);
      const int mask = _mm_movemask_pd(vcmp);
      if (LIKELY(!mask))
        continue;
      // It's important to extract pointers from the already loaded vector to
      // avoid racing with the mutator.
      if (mask & 0b01) {
        quarantine_size +=
            pcscan_task_.TryMarkObjectInNormalBucketPool<GigaCageLookupPolicy>(
                _mm_cvtsi128_si64(_mm_castpd_si128(maybe_ptrs)));
      }
      if (mask & 0b10) {
        // Extraction intrinsics for qwords are only supported in SSE4.1, so
        // instead we reshuffle dwords with pshufd. The mask is used to move the
        // 4th and 3rd dwords into the second and first position.
        static constexpr int kSecondWordMask = (3 << 2) | (2 << 0);
        const __m128i shuffled =
            _mm_shuffle_epi32(_mm_castpd_si128(maybe_ptrs), kSecondWordMask);
        quarantine_size +=
            pcscan_task_.TryMarkObjectInNormalBucketPool<GigaCageLookupPolicy>(
                _mm_cvtsi128_si64(shuffled));
      }
    }
    return quarantine_size;
  }

  __attribute__((target("avx2"))) NO_SANITIZE("thread") size_t
      RunAVX2(uintptr_t* begin, uintptr_t* end) const {
    static constexpr size_t kAlignmentRequirement = 32;
    static constexpr size_t kWordsInVector = 4;
    PA_DCHECK(!(reinterpret_cast<uintptr_t>(begin) % kAlignmentRequirement));
    // For AVX2, stick to integer instructions. This brings slightly better
    // throughput. For example, according to the Intel docs, on Broadwell and
    // Haswell the CPI of vmovdqa (_mm256_load_si256) is twice smaller (0.25)
    // than that of vmovapd (_mm256_load_pd).
    const __m256i vbase = _mm256_set1_epi64x(normal_bucket_pool_base_);
    const __m256i cage_mask =
        _mm256_set1_epi64x(PartitionAddressSpace::NormalBucketPoolBaseMask());

    size_t quarantine_size = 0;
    uintptr_t* payload = begin;
    for (; payload < (end - kWordsInVector); payload += kWordsInVector) {
      const __m256i maybe_ptrs =
          _mm256_load_si256(reinterpret_cast<__m256i*>(payload));
      const __m256i vand = _mm256_and_si256(maybe_ptrs, cage_mask);
      const __m256i vcmp = _mm256_cmpeq_epi64(vand, vbase);
      const int mask = _mm256_movemask_pd(_mm256_castsi256_pd(vcmp));
      if (LIKELY(!mask))
        continue;
      // It's important to extract pointers from the already loaded vector to
      // avoid racing with the mutator.
      if (mask & 0b0001)
        quarantine_size +=
            pcscan_task_.TryMarkObjectInNormalBucketPool<GigaCageLookupPolicy>(
                _mm256_extract_epi64(maybe_ptrs, 0));
      if (mask & 0b0010)
        quarantine_size +=
            pcscan_task_.TryMarkObjectInNormalBucketPool<GigaCageLookupPolicy>(
                _mm256_extract_epi64(maybe_ptrs, 1));
      if (mask & 0b0100)
        quarantine_size +=
            pcscan_task_.TryMarkObjectInNormalBucketPool<GigaCageLookupPolicy>(
                _mm256_extract_epi64(maybe_ptrs, 2));
      if (mask & 0b1000)
        quarantine_size +=
            pcscan_task_.TryMarkObjectInNormalBucketPool<GigaCageLookupPolicy>(
                _mm256_extract_epi64(maybe_ptrs, 3));
    }

    quarantine_size += RunUnvectorized(payload, end);
    return quarantine_size;
  }
#endif  // defined(ARCH_CPU_X86_64)

  ALWAYS_INLINE NO_SANITIZE("thread") size_t
      RunUnvectorized(uintptr_t* begin, uintptr_t* end) const {
    PA_DCHECK(!(reinterpret_cast<uintptr_t>(begin) % sizeof(uintptr_t)));
    size_t quarantine_size = 0;
    for (; begin < end; ++begin) {
      uintptr_t maybe_ptr = *begin;
#if defined(PA_HAS_64_BITS_POINTERS)
      // On 64bit architectures, call IsInNormalBucketPool instead of
      // IsManagedByPartitionAllocNormalBuckets to avoid redundant loads of
      // PartitionAddressSpace::normal_bucket_pool_base_address_.
      if (LIKELY(!IsInNormalBucketPool(maybe_ptr)))
        continue;
#endif
      quarantine_size +=
          pcscan_task_.TryMarkObjectInNormalBucketPool<GigaCageLookupPolicy>(
              maybe_ptr);
    }
    return quarantine_size;
  }

  ALWAYS_INLINE NO_SANITIZE("thread") size_t
      RunUnvectorizedNoGigaCage(uintptr_t* begin, uintptr_t* end) const {
    PA_DCHECK(!(reinterpret_cast<uintptr_t>(begin) % sizeof(uintptr_t)));
    size_t quarantine_size = 0;
    for (; begin < end; ++begin) {
      uintptr_t maybe_ptr = *begin;
      if (!maybe_ptr)
        continue;
      quarantine_size +=
          pcscan_task_.TryMarkObjectInNormalBucketPool<NoGigaCageLookupPolicy>(
              maybe_ptr);
    }
    return quarantine_size;
  }

  const ScanFunction scan_function_;
  const PCScanTask& pcscan_task_;
#if defined(PA_HAS_64_BITS_POINTERS)
  // Keep this a constant so that the compiler can remove redundant loads for
  // the base of the normal bucket pool and hoist them out of the loops.
  const uintptr_t normal_bucket_pool_base_;
#endif
};

PCScanTask::PCScanTask(PCScan& pcscan)
    : pcscan_epoch_(pcscan.quarantine_data_.epoch()),
      stats_(PCScanInternal::Instance().process_name()),
      pcscan_(pcscan) {}

void PCScanTask::ScanPartitions() {
  const ScanLoop scan_loop(*this);
  // For scanning large areas, it's worthwhile checking whether the range that
  // is scanned contains quarantined objects.
  size_t quarantine_size = 0;
  for (auto scan_area : snapshot_.large_scan_areas()) {
    // The bitmap is (a) always guaranteed to exist and (b) the same for all
    // objects in a given slot span.
    // TODO(chromium:1129751): Check mutator bitmap as well if performance
    // allows.
    auto* bitmap = QuarantineBitmapFromPointer(
        QuarantineBitmapType::kScanner, pcscan_epoch_,
        reinterpret_cast<char*>(scan_area.begin));
    for (uintptr_t* current_slot = scan_area.begin;
         current_slot < scan_area.end;
         current_slot += (scan_area.slot_size / sizeof(uintptr_t))) {
      // It is okay to skip objects as their payload has been zapped at this
      // point which means that the pointers no longer retain other objects.
      if (bitmap->CheckBit(reinterpret_cast<uintptr_t>(current_slot))) {
        continue;
      }
      uintptr_t* current_slot_end =
          current_slot + (scan_area.slot_size / sizeof(uintptr_t));
      PA_DCHECK(current_slot_end <= scan_area.end);
      quarantine_size += scan_loop.Run(current_slot, current_slot_end);
    }
  }
  // Scan areas with regular size slots.
  for (auto scan_area : snapshot_.scan_areas()) {
    quarantine_size += scan_loop.Run(scan_area.begin, scan_area.end);
  }
  stats_.IncreaseSurvivedQuarantineSize(quarantine_size);
}

void PCScanTask::SweepQuarantine() {
  using AccessType = QuarantineBitmap::AccessType;

  const bool giga_cage_enabled = features::IsPartitionAllocGigaCageEnabled();
  size_t swept_bytes = 0;

  for (uintptr_t super_page : snapshot_.quarantinable_super_pages()) {
    auto* bitmap = QuarantineBitmapFromPointer(
        QuarantineBitmapType::kScanner, pcscan_epoch_,
        reinterpret_cast<char*>(super_page));
    auto* root = Root::FromSuperPage(reinterpret_cast<char*>(super_page));
    bitmap->template IterateAndClear<AccessType::kNonAtomic>(
        [root, giga_cage_enabled, &swept_bytes](uintptr_t ptr) {
          auto* object = reinterpret_cast<void*>(ptr);
          auto* slot_span = SlotSpan::FromSlotInnerPtr(object);
          swept_bytes += slot_span->bucket->slot_size;
          root->FreeNoHooksImmediate(object, slot_span);
#if defined(PA_HAS_64_BITS_POINTERS)
          if (giga_cage_enabled) {
            // Reset card(s) for this quarantined object. Please note that the
            // cards may still contain quarantined objects (which were promoted
            // in this scan cycle), but
            // ClearQuarantinedObjectsAndFilterSuperPages() will set them again
            // in the next PCScan cycle.
            QuarantineCardTable::GetFrom(ptr).Unquarantine(
                ptr, slot_span->GetUsableSize(root));
          }
#else
          (void)giga_cage_enabled;
#endif
        });
  }

  stats_.IncreaseSweptSize(swept_bytes);
}

void PCScanTask::FinishScanner() {
  stats_.UpdateHistograms();
  LogStats(stats_.swept_size(), pcscan_.quarantine_data_.last_size(),
           stats_.survived_quarantine_size());

  const size_t total_pa_heap_size =
      PCScanInternal::Instance().CalculateTotalHeapSize();

  pcscan_.quarantine_data_.Account(stats_.survived_quarantine_size());
  pcscan_.quarantine_data_.GrowLimitIfNeeded(total_pa_heap_size);

  // Check that concurrent task can't be scheduled twice.
  PA_CHECK(pcscan_.state_.exchange(PCScan::State::kNotRunning,
                                   std::memory_order_acq_rel) ==
           PCScan::State::kSweepingAndFinishing);
}

void PCScanTask::RunFromScanner() {
  {
    StatsCollector::ScannerScope overall_scope(
        stats_, StatsCollector::ScannerId::kOverall);
    // Take snapshot of partition-alloc heap.
    snapshot_.Take(pcscan_epoch_);
    {
      // Clear all quarantined objects and prepare the card table.
      StatsCollector::ScannerScope clear_scope(
          stats_, StatsCollector::ScannerId::kClear);
      ClearQuarantinedObjectsAndPrepareCardTable();
    }
    {
      // Scan heap for dangling references.
      StatsCollector::ScannerScope scan_scope(stats_,
                                              StatsCollector::ScannerId::kScan);
      ScanPartitions();
    }
    pcscan_.state_.store(PCScan::State::kSweepingAndFinishing,
                         std::memory_order_relaxed);
    {
      // Sweep unreachable quarantined objects.
      StatsCollector::ScannerScope sweep_scope(
          stats_, StatsCollector::ScannerId::kSweep);
      SweepQuarantine();
    }
  }
  FinishScanner();
}

class PCScan::PCScanThread final {
 public:
  using TaskHandle = std::unique_ptr<PCScanTask>;

  static PCScanThread& Instance() {
    // Lazily instantiate the scanning thread.
    static base::NoDestructor<PCScanThread> instance;
    return *instance;
  }

  void PostTask(TaskHandle task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      PA_DCHECK(!posted_task_.get());
      posted_task_ = std::move(task);
    }
    condvar_.notify_all();
  }

 private:
  friend class base::NoDestructor<PCScanThread>;

  PCScanThread() {
    std::thread{[this] {
      static constexpr const char* kThreadName = "PCScan";
      // Ideally we should avoid mixing base:: and std:: API for threading, but
      // this is useful for visualizing the pcscan thread in chrome://tracing.
      base::PlatformThread::SetName(kThreadName);
      TaskLoop();
    }}.detach();
  }

  void TaskLoop() {
    while (true) {
      TaskHandle current_task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        condvar_.wait(lock, [this] { return posted_task_.get(); });
        std::swap(current_task, posted_task_);
      }
      std::move(*current_task).RunFromScanner();
    }
  }

  std::mutex mutex_;
  std::condition_variable condvar_;
  TaskHandle posted_task_;
};

constexpr size_t PCScan::QuarantineData::kQuarantineSizeMinLimit;

void PCScan::QuarantineData::ResetAndAdvanceEpoch() {
  last_size_ = current_size_.exchange(0, std::memory_order_relaxed);
  epoch_.fetch_add(1, std::memory_order_relaxed);
}

void PCScan::QuarantineData::GrowLimitIfNeeded(size_t heap_size) {
  static constexpr double kQuarantineSizeFraction = 0.1;
  // |heap_size| includes the current quarantine size, we intentionally leave
  // some slack till hitting the limit.
  size_limit_.store(
      std::max(kQuarantineSizeMinLimit,
               static_cast<size_t>(kQuarantineSizeFraction * heap_size)),
      std::memory_order_relaxed);
}

void PCScan::PerformScan(InvocationMode invocation_mode) {
#if DCHECK_IS_ON()
  const auto& internal = PCScanInternal::Instance();
  const auto& scannable_roots = internal.scannable_roots();
  const auto& nonscannable_roots = internal.nonscannable_roots();
  PA_DCHECK(scannable_roots.size() > 0);
  PA_DCHECK(std::all_of(scannable_roots.begin(), scannable_roots.end(),
                        [](Root* root) { return root->IsScanEnabled(); }));
  PA_DCHECK(
      std::all_of(nonscannable_roots.begin(), nonscannable_roots.end(),
                  [](Root* root) { return root->IsQuarantineEnabled(); }));
#endif

  {
    // If scanning is already in progress, bail out.
    State expected = State::kNotRunning;
    if (!state_.compare_exchange_strong(expected, State::kScheduled,
                                        std::memory_order_acq_rel,
                                        std::memory_order_relaxed))
      return;
  }

  quarantine_data_.ResetAndAdvanceEpoch();

  // Create PCScan task.
  auto task = std::make_unique<PCScanTask>(*this);

  state_.store(State::kScanning, std::memory_order_release);

  // Post PCScan task.
  if (LIKELY(invocation_mode == InvocationMode::kNonBlocking)) {
    PCScanThread::Instance().PostTask(std::move(task));
  } else {
    PA_DCHECK(InvocationMode::kBlocking == invocation_mode ||
              InvocationMode::kForcedBlocking == invocation_mode);
    std::move(*task).RunFromScanner();
  }
}

void PCScan::PerformScanIfNeeded(InvocationMode invocation_mode) {
  if (!PCScanInternal::Instance().scannable_roots().size())
    return;
  if (invocation_mode == InvocationMode::kForcedBlocking ||
      quarantine_data_.MinimumScanningThresholdReached())
    PerformScan(invocation_mode);
}

void PCScan::RegisterScannableRoot(Root* root) {
  PCScanInternal::Instance().RegisterScannableRoot(root);
}

void PCScan::RegisterNonScannableRoot(Root* root) {
  PCScanInternal::Instance().RegisterNonScannableRoot(root);
}

void PCScan::SetProcessName(const char* process_name) {
  PCScanInternal::Instance().SetProcessName(process_name);
}

void PCScan::ClearRootsForTesting() {
  PCScanInternal::Instance().ClearRootsForTesting();  // IN-TEST
}

PCScan PCScan::instance_ PA_CONSTINIT;

}  // namespace internal
}  // namespace base
