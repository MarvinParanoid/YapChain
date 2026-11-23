// SPDX-License-Identifier: Apache-2.0
#include "yap/crypto/ed25519.hpp"

#include <sodium.h>

#include <new>
#include <stdexcept>
#include <utility>

#include "yap/crypto/sodium.hpp"

namespace yap::crypto {

static_assert(crypto_sign_SEEDBYTES == kSeedSize);
static_assert(crypto_sign_PUBLICKEYBYTES == kPublicKeySize);
static_assert(crypto_sign_BYTES == kSignatureSize);
static_assert(crypto_sign_SECRETKEYBYTES == 64);

namespace {
unsigned char* allocate_secret() {
    auto* p = static_cast<unsigned char*>(sodium_malloc(crypto_sign_SECRETKEYBYTES));
    if (p == nullptr) {
        throw std::bad_alloc();
    }
    return p;
}
}  // namespace

SecretKey SecretKey::from_seed(const Seed& seed) {
    ensure_initialized();
    SecretKey key;
    key.secret_ = allocate_secret();
    if (crypto_sign_seed_keypair(key.public_key_.data(), key.secret_, seed.data()) != 0) {
        throw std::runtime_error("crypto_sign_seed_keypair failed");
    }
    return key;
}

SecretKey SecretKey::generate() {
    ensure_initialized();
    SecretKey key;
    key.secret_ = allocate_secret();
    if (crypto_sign_keypair(key.public_key_.data(), key.secret_) != 0) {
        throw std::runtime_error("crypto_sign_keypair failed");
    }
    return key;
}

SecretKey::SecretKey(SecretKey&& other) noexcept
    : secret_(std::exchange(other.secret_, nullptr)), public_key_(other.public_key_) {}

SecretKey& SecretKey::operator=(SecretKey&& other) noexcept {
    if (this != &other) {
        sodium_free(secret_);
        secret_ = std::exchange(other.secret_, nullptr);
        public_key_ = other.public_key_;
    }
    return *this;
}

SecretKey::~SecretKey() { sodium_free(secret_); }

Signature SecretKey::sign(ByteView message) const {
    if (secret_ == nullptr) {
        throw std::logic_error("SecretKey used after move");
    }
    Signature sig{};
    if (crypto_sign_detached(sig.data(), nullptr, message.data(), message.size(), secret_) != 0) {
        throw std::runtime_error("crypto_sign_detached failed");
    }
    return sig;
}

Signature SecretKey::sign_prefixed(std::string_view prefix, ByteView payload) const {
    return sign(concat(as_bytes(prefix), payload));
}

bool verify(const PublicKey& public_key, ByteView message, const Signature& signature) {
    ensure_initialized();
    return crypto_sign_verify_detached(signature.data(), message.data(), message.size(), public_key.data()) == 0;
}

bool verify_prefixed(const PublicKey& public_key, std::string_view prefix, ByteView payload,
                     const Signature& signature) {
    return verify(public_key, concat(as_bytes(prefix), payload), signature);
}

bool is_valid_validator_public_key(const PublicKey& public_key) {
    ensure_initialized();
    return crypto_core_ed25519_is_valid_point(public_key.data()) == 1;
}

}  // namespace yap::crypto
