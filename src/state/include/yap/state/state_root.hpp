// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "yap/state/kv.hpp"

namespace yap::state {

/// state_root of every live (raw_key, value) pair in `kv` (protocol.md §8.1).
[[nodiscard]] Hash32 state_root(const KvReader& kv);

}  // namespace yap::state
