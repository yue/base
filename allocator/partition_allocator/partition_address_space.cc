// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/allocator/partition_allocator/partition_address_space.h"

#include "base/allocator/partition_allocator/page_allocator.h"
#include "base/allocator/partition_allocator/page_allocator_internal.h"
#include "base/allocator/partition_allocator/partition_alloc_check.h"
#include "base/allocator/partition_allocator/partition_alloc_constants.h"
#include "base/bits.h"

namespace base {

namespace internal {

#if defined(__LP64__)

uintptr_t PartitionAddressSpace::reserved_address_start_ = 0;
// Before PartitionAddressSpace::Init(), no allocation are allocated from a
// reserved address space. So initially make reserved_base_address_ to
// be kReservedAddressSpaceOffsetMask. So PartitionAddressSpace::Contains()
// always returns false.
// Do something similar for normal_bucket_pool_base_address_.
uintptr_t PartitionAddressSpace::reserved_base_address_ =
    kReservedAddressSpaceOffsetMask;
uintptr_t PartitionAddressSpace::normal_bucket_pool_base_address_ =
    kNormalBucketPoolOffsetMask;

pool_handle PartitionAddressSpace::direct_map_pool_ = 0;
pool_handle PartitionAddressSpace::normal_bucket_pool_ = 0;

void PartitionAddressSpace::Init() {
  PA_DCHECK(!reserved_address_start_);
  reserved_address_start_ = reinterpret_cast<uintptr_t>(SystemAllocPages(
      nullptr, kReservedAddressSpaceSize, base::PageInaccessible,
      PageTag::kPartitionAlloc, false));
  PA_CHECK(reserved_address_start_);

  const uintptr_t reserved_address_end =
      reserved_address_start_ + kReservedAddressSpaceSize;

  reserved_base_address_ =
      bits::Align(reserved_address_start_, kReservedAddressSpaceAlignment);
  PA_DCHECK(reserved_base_address_ >= reserved_address_start_);
  PA_DCHECK(!(reserved_base_address_ & kReservedAddressSpaceOffsetMask));

  uintptr_t current = reserved_base_address_;

  direct_map_pool_ = internal::AddressPoolManager::GetInstance()->Add(
      current, kDirectMapPoolSize);
  PA_DCHECK(direct_map_pool_);
  current += kDirectMapPoolSize;

  normal_bucket_pool_base_address_ = current;
  normal_bucket_pool_ = internal::AddressPoolManager::GetInstance()->Add(
      current, kNormalBucketPoolSize);
  PA_DCHECK(normal_bucket_pool_);
  current += kNormalBucketPoolSize;
  PA_DCHECK(current <= reserved_address_end);
  PA_DCHECK(current == reserved_base_address_ + kDesiredAddressSpaceSize);
}

void PartitionAddressSpace::UninitForTesting() {
  PA_DCHECK(reserved_address_start_);
  FreePages(reinterpret_cast<void*>(reserved_address_start_),
            kReservedAddressSpaceSize);
  reserved_address_start_ = 0;
  reserved_base_address_ = kReservedAddressSpaceOffsetMask;
  direct_map_pool_ = 0;
  normal_bucket_pool_ = 0;
  internal::AddressPoolManager::GetInstance()->ResetForTesting();
}

#endif  // defined(__LP64__)

}  // namespace internal

}  // namespace base
