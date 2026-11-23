// SPDX-License-Identifier: Apache-2.0
#include "yap/crypto/sodium.hpp"

#include <sodium.h>

#include <stdexcept>

namespace yap::crypto {

void ensure_initialized() {
    // Function-local static: initialised once, thread-safe (C++11 [stmt.dcl]/4).
    static const int rc = sodium_init();
    if (rc < 0) {
        throw std::runtime_error("libsodium initialisation failed");
    }
}

}  // namespace yap::crypto
