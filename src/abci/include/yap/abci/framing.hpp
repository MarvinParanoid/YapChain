// SPDX-License-Identifier: Apache-2.0
//
// ABCI socket framing (CometBFT v0.40.0 abci/types/messages.go): every message is a protobuf
// Request/Response prefixed with its length as an unsigned varint (binary.PutUvarint).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace yap::abci {

/// Default cap on one message. CometBFT blocks are at most 100 MB (hard cap of BlockParams.MaxBytes);
/// a RequestFinalizeBlock carrying a full block stays below this.
inline constexpr std::size_t kDefaultMaxMessageBytes = 128 * 1024 * 1024;

void append_uvarint(std::string& out, std::uint64_t value);

/// Incremental frame decoder. Memory grows only with bytes actually received: a declared length is
/// checked against the limit but never pre-allocated.
class FrameDecoder {
public:
    explicit FrameDecoder(std::size_t max_message_bytes = kDefaultMaxMessageBytes) : max_(max_message_bytes) {}

    enum class Status { NeedMore, Frame, Error };

    void feed(const char* data, std::size_t size);
    /// Extracts the next complete frame into `frame`. After Error the decoder stays in error.
    Status next(std::string& frame);
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    [[nodiscard]] std::size_t buffered() const noexcept { return buf_.size() - pos_; }

private:
    std::size_t max_;
    std::string buf_;
    std::size_t pos_ = 0;
    std::string error_;
};

}  // namespace yap::abci
