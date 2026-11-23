// SPDX-License-Identifier: Apache-2.0
#include "yap/crypto/hash.hpp"

#include <sodium.h>

#include <stdexcept>

#include "yap/crypto/sodium.hpp"

namespace yap::crypto {

static_assert(crypto_hash_sha256_BYTES == 32);

struct Sha256::State {
    crypto_hash_sha256_state st{};
};

Sha256::Sha256() : state_(std::make_unique<State>()) {
    ensure_initialized();
    crypto_hash_sha256_init(&state_->st);
}

Sha256::~Sha256() {
    if (state_) {
        sodium_memzero(&state_->st, sizeof state_->st);
    }
}

Sha256::Sha256(Sha256&&) noexcept = default;
Sha256& Sha256::operator=(Sha256&&) noexcept = default;

Sha256& Sha256::update(ByteView data) {
    if (!state_) {
        throw std::logic_error("Sha256 used after finalize or move");
    }
    crypto_hash_sha256_update(&state_->st, data.data(), data.size());
    return *this;
}

Hash32 Sha256::finalize() {
    if (!state_) {
        throw std::logic_error("Sha256 used after finalize or move");
    }
    Hash32 out{};
    crypto_hash_sha256_final(&state_->st, out.data());
    state_.reset();
    return out;
}

Hash32 sha256(ByteView data) {
    ensure_initialized();
    Hash32 out{};
    crypto_hash_sha256(out.data(), data.data(), data.size());
    return out;
}

namespace {
void check_tag(std::string_view tag) {
    if (tag.empty() || tag.size() > 255) {
        throw std::invalid_argument("hash tag must be 1..255 bytes");
    }
    for (const char c : tag) {
        const auto b = static_cast<unsigned char>(c);
        if (b < 0x21 || b > 0x7E) {
            throw std::invalid_argument("hash tag must be printable ASCII without spaces");
        }
    }
}
}  // namespace

TaggedHasher::TaggedHasher(std::string_view tag) {
    check_tag(tag);
    const Byte len = static_cast<Byte>(tag.size());
    inner_.update(ByteView(&len, 1));
    inner_.update(as_bytes(tag));
}

TaggedHasher& TaggedHasher::update(ByteView data) {
    inner_.update(data);
    return *this;
}

Hash32 TaggedHasher::finalize() { return inner_.finalize(); }

Hash32 tagged_hash(std::string_view tag, ByteView msg) { return TaggedHasher(tag).update(msg).finalize(); }

}  // namespace yap::crypto
