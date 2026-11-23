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
    /// Same, reading the seed from caller-managed (e.g. guarded) memory without copying it.
    [[nodiscard]] static SecretKey from_seed_bytes(const unsigned char* seed32);
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

/// Validator consensus public key check (docs/protocol.md §5.7, ADR-0020): canonical encoding,
/// not of small order, on the curve and in the prime-order subgroup — the semantics of
/// libsodium crypto_core_ed25519_is_valid_point(). Stricter than what verify() accepts for
/// user keys (mixed-order keys are rejected here); do not use it to validate user accounts.
[[nodiscard]] bool is_valid_validator_public_key(const PublicKey& public_key);

}  // namespace yap::crypto
