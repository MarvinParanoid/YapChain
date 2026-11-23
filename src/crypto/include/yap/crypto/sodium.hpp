// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace yap::crypto {

/// Initialises libsodium exactly once (thread-safe). Throws std::runtime_error on failure.
/// Every public crypto function calls it, so explicit calls are optional.
void ensure_initialized();

}  // namespace yap::crypto
