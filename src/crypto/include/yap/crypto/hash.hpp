// SPDX-License-Identifier: Apache-2.0
//
// SHA-256 and tagged hashing (docs/protocol.md §4).
#pragma once

#include <array>
#include <memory>
#include <string_view>

#include "yap/common/bytes.hpp"

namespace yap::crypto {

[[nodiscard]] Hash32 sha256(ByteView data);

/// Incremental SHA-256.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    Sha256(Sha256&&) noexcept;
    Sha256& operator=(Sha256&&) noexcept;

    Sha256& update(ByteView data);
    /// Produces the digest; the object must not be used afterwards.
    [[nodiscard]] Hash32 finalize();

private:
    struct State;
    std::unique_ptr<State> state_;
};

/// H_tag(tag, msg) = SHA-256(u8(len(tag)) || tag || msg).
/// `tag` must be 1..255 bytes of ASCII 0x21..0x7E; otherwise std::invalid_argument
/// (tags are protocol constants, so this is a programming error).
[[nodiscard]] Hash32 tagged_hash(std::string_view tag, ByteView msg);

/// Incremental tagged hash: the prefix is absorbed on construction.
class TaggedHasher {
public:
    explicit TaggedHasher(std::string_view tag);
    TaggedHasher& update(ByteView data);
    [[nodiscard]] Hash32 finalize();

private:
    Sha256 inner_;
};

}  // namespace yap::crypto
