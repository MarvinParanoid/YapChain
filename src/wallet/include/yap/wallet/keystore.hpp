// SPDX-License-Identifier: Apache-2.0
//
// Encrypted keystore (protocol.md §5.6, ADR-0024): one Ed25519 seed per file, encrypted with
// XChaCha20-Poly1305 under a key derived by Argon2id. All non-secret header fields are bound to
// the ciphertext as associated data, so editing any of them makes decryption fail.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "yap/common/result.hpp"
#include "yap/crypto/address.hpp"
#include "yap/crypto/ed25519.hpp"

namespace yap::wallet {

/// What a key may be used for. Keys of different purposes live in different directories and
/// are never accepted in place of each other (validator consensus keys are not handled at all:
/// they belong to CometBFT).
enum class KeyPurpose : std::uint8_t { Account = 1, DevProducer = 2 };
[[nodiscard]] std::string_view to_string(KeyPurpose p) noexcept;

struct KdfParams {
    std::uint64_t opslimit = 0;
    std::uint64_t memlimit = 0;
    /// protocol.md §5.6: crypto_pwhash MODERATE (used by the CLI).
    [[nodiscard]] static KdfParams moderate() noexcept;
    /// Weaker but still accepted level, for tests only.
    [[nodiscard]] static KdfParams interactive() noexcept;
};

enum class KeystoreError : std::uint8_t {
    Malformed = 1,          ///< not a keystore: bad JSON, unknown/missing fields, bad encodings
    UnsupportedVersion,
    UnsupportedKdf,         ///< kdf name or parameters outside the accepted range
    WrongPurpose,
    WrongPasswordOrCorrupted,  ///< authenticated decryption failed (indistinguishable by design)
    Inconsistent,           ///< decrypted seed does not match the stored public key / address
};
[[nodiscard]] std::string_view to_string(KeystoreError e) noexcept;

/// Public, non-secret part of a keystore (readable without the password).
struct KeyInfo {
    KeyPurpose purpose = KeyPurpose::Account;
    crypto::PublicKey public_key{};
    crypto::AccountId account_id{};
    std::string address;
};

/// Creates a keystore document for a fresh seed from the OS CSPRNG. The secret key is returned
/// so the caller can use it immediately; the seed itself never leaves guarded memory.
struct NewKey {
    std::string json;
    KeyInfo info;
};
[[nodiscard]] NewKey create_keystore(std::string_view password, KeyPurpose purpose, const KdfParams& kdf);

/// Strict parsing of the public part.
[[nodiscard]] Result<KeyInfo, KeystoreError> read_key_info(std::string_view json);

/// Decrypts the seed and returns the signing key. `expected` must match the stored purpose.
[[nodiscard]] Result<crypto::SecretKey, KeystoreError> unlock_keystore(std::string_view json, std::string_view password,
                                                                        KeyPurpose expected);

// ------------------------------------------------------------------ files

/// Writes `content` to `path` atomically with mode 0600, refusing to overwrite an existing file.
/// Throws std::runtime_error.
void write_private_file(const std::filesystem::path& path, std::string_view content);
/// Reads a key file; throws std::runtime_error if it is missing or accessible by group/others.
[[nodiscard]] std::string read_private_file(const std::filesystem::path& path);

/// Key names: [a-z0-9_-]{1,32}.
[[nodiscard]] bool valid_key_name(std::string_view name) noexcept;

}  // namespace yap::wallet
