// SPDX-License-Identifier: Apache-2.0
#include "yap/abci/framing.hpp"

namespace yap::abci {

void append_uvarint(std::string& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7F) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

void FrameDecoder::feed(const char* data, std::size_t size) {
    if (!error_.empty()) return;
    // Drop consumed bytes before growing, so the buffer holds at most one partial frame (+ new data).
    if (pos_ > 0 && (pos_ > 65536 || pos_ == buf_.size())) {
        buf_.erase(0, pos_);
        pos_ = 0;
    }
    buf_.append(data, size);
}

FrameDecoder::Status FrameDecoder::next(std::string& frame) {
    if (!error_.empty()) return Status::Error;
    std::uint64_t len = 0;
    std::size_t i = pos_;
    unsigned shift = 0;
    for (;;) {
        if (i == buf_.size()) return Status::NeedMore;
        const auto b = static_cast<unsigned char>(buf_[i]);
        ++i;
        if (shift == 63 && b > 1) {
            error_ = "malformed varint length prefix (overflow)";
            return Status::Error;
        }
        len |= static_cast<std::uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
        if (shift > 63) {
            error_ = "malformed varint length prefix (too long)";
            return Status::Error;
        }
    }
    if (len > max_) {
        error_ = "message of " + std::to_string(len) + " bytes exceeds the limit of " + std::to_string(max_);
        return Status::Error;
    }
    if (buf_.size() - i < len) return Status::NeedMore;  // wait; nothing allocated for `len`
    frame.assign(buf_, i, static_cast<std::size_t>(len));
    pos_ = i + static_cast<std::size_t>(len);
    return Status::Frame;
}

}  // namespace yap::abci
