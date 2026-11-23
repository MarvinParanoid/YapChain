// SPDX-License-Identifier: Apache-2.0
//
// Ed25519 (RFC 8032) via libsodium. Verification semantics are normative:
// docs/protocol.md §5.2 / ADR-0014, locked by test-vectors/ed25519.json.
#pragma once

#include <cstddef>
#include <string_view>

#include "yap/common/bytes.hpp"

namespace yap::crypto {

inline constexpr std::size_t kSeedSize = 32;
inline constexpr std::size_t kPublicKeySize = 32;
inline constexpr std::size_t kSignatureSize = 64;

using Seed = FixedBytes<kSeedSize>;
using PublicKey = FixedBytes<kPublicKeySize>;
using Signature = FixedBytes<kSignatureSize>;

/// Ed25519 signing key. Secret material lives in libsodium guarded memory
/// (sodium_malloc: guard pages, mlock, zeroed on free). Move-only.
class SecretKey {
public:
    /// Deterministic key from a 32-byte seed.
    [[nodiscard]] static SecretKey from_seed(const Seed& seed);
    /// Fresh key from the OS CSPRNG.
    [[nodiscard]] static SecretKey generate();

    SecretKey(const SecretKey&) = delete;
    SecretKey& operator=(const SecretKey&) = delete;
    SecretKey(SecretKey&& other) noexcept;
    SecretKey& operator=(SecretKey&& other) noexcept;
    ~SecretKey();

    [[nodiscard]] const PublicKey& public_key() const noexcept { return public_key_; }

    /// Signs `message` as is.
    [[nodiscard]] Signature sign(ByteView message) const;
    /// Signs `prefix || payload` (domain-separated, docs/protocol.md §4.4).
    [[nodiscard]] Signature sign_prefixed(std::string_view prefix, ByteView payload) const;

private:
    SecretKey() = default;
    unsigned char* secret_ = nullptr;  // 64 bytes: seed || public key (libsodium layout)
    PublicKey public_key_{};
};

/// docs/protocol.md §5.2. Never throws on malformed input; returns false.
[[nodiscard]] bool verify(const PublicKey& public_key, ByteView message, const Signature& signature);
[[nodiscard]] bool verify_prefixed(const PublicKey& public_key, std::string_view prefix, ByteView payload,
                                   const Signature& signature);

}  // namespace yap::crypto
