// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "partition_alloc/partition_alloc_base/process/process_handle.h"

#include <unistd.h>

namespace partition_alloc::internal::base {

ProcessId GetCurrentProcId() {
  return getpid();
}

}  // namespace partition_alloc::internal::base
