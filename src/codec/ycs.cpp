// SPDX-License-Identifier: Apache-2.0
#include "yap/codec/ycs.hpp"

#include <limits>
#include <stdexcept>

namespace yap::ycs {

std::string_view to_string(DecodeError e) noexcept {
    switch (e) {
        case DecodeError::Truncated: return "truncated";
        case DecodeError::TrailingBytes: return "trailing_bytes";
        case DecodeError::InvalidBool: return "invalid_bool";
        case DecodeError::InvalidOptionTag: return "invalid_option_tag";
        case DecodeError::InvalidEnumTag: return "invalid_enum_tag";
        case DecodeError::LengthLimit: return "length_limit";
        case DecodeError::InvalidContent: return "invalid_content";
    }
    return "unknown";
}

// ------------------------------------------------------------------ Writer

namespace {
void put_le(Bytes& buf, std::uint64_t v, std::size_t width) {
    for (std::size_t i = 0; i < width; ++i) {
        buf.push_back(static_cast<Byte>(v >> (8 * i)));
    }
}

std::uint32_t checked_u32_length(std::size_t n) {
    if (n > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("YCS length exceeds u32");
    }
    return static_cast<std::uint32_t>(n);
}
}  // namespace

void Writer::u8(std::uint8_t v) { buf_.push_back(v); }
void Writer::u16(std::uint16_t v) { put_le(buf_, v, 2); }
void Writer::u32(std::uint32_t v) { put_le(buf_, v, 4); }
void Writer::u64(std::uint64_t v) { put_le(buf_, v, 8); }
void Writer::boolean(bool v) { buf_.push_back(v ? Byte{1} : Byte{0}); }
void Writer::fixed(ByteView v) { buf_.insert(buf_.end(), v.begin(), v.end()); }

void Writer::bytes(ByteView v) {
    u32(checked_u32_length(v.size()));
    fixed(v);
}

void Writer::string(std::string_view v) { bytes(as_bytes(v)); }
void Writer::length(std::size_t count) { u32(checked_u32_length(count)); }
void Writer::option_tag(bool present) { boolean(present); }
void Writer::enum_tag(std::uint8_t tag) { u8(tag); }

// ------------------------------------------------------------------ Reader

void Reader::fail(DecodeError e) noexcept {
    if (!error_) {
        error_ = e;
    }
}

const Byte* Reader::consume(std::size_t n) noexcept {
    if (error_) {
        return nullptr;
    }
    if (n > remaining()) {
        fail(DecodeError::Truncated);
        return nullptr;
    }
    const Byte* p = in_.data() + pos_;
    pos_ += n;
    return p;
}

std::uint64_t Reader::uint_le(std::size_t width) noexcept {
    const Byte* p = consume(width);
    if (p == nullptr) {
        return 0;
    }
    std::uint64_t v = 0;
    for (std::size_t i = 0; i < width; ++i) {
        v |= static_cast<std::uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

std::uint8_t Reader::u8() noexcept { return static_cast<std::uint8_t>(uint_le(1)); }
std::uint16_t Reader::u16() noexcept { return static_cast<std::uint16_t>(uint_le(2)); }
std::uint32_t Reader::u32() noexcept { return static_cast<std::uint32_t>(uint_le(4)); }
std::uint64_t Reader::u64() noexcept { return uint_le(8); }

bool Reader::boolean() noexcept {
    const std::uint8_t b = u8();
    if (b > 1) {
        fail(DecodeError::InvalidBool);
        return false;
    }
    return b == 1;
}

std::uint32_t Reader::byte_length(std::uint32_t max_len) noexcept {
    return vec_length(max_len, 1);
}

std::uint32_t Reader::vec_length(std::uint32_t max_count, std::size_t min_element_size) noexcept {
    const std::uint32_t n = u32();
    if (error_) {
        return 0;
    }
    if (n > max_count) {
        fail(DecodeError::LengthLimit);
        return 0;
    }
    // n <= 2^32-1 and min_element_size is small, so the division form avoids overflow.
    if (min_element_size != 0 && n > remaining() / min_element_size) {
        fail(DecodeError::Truncated);
        return 0;
    }
    return n;
}

Bytes Reader::bytes(std::uint32_t max_len) {
    const std::uint32_t n = byte_length(max_len);
    const Byte* p = consume(n);
    if (p == nullptr) {
        return {};
    }
    return Bytes(p, p + n);
}

std::string Reader::string(std::uint32_t max_len) {
    const std::uint32_t n = byte_length(max_len);
    const Byte* p = consume(n);
    if (p == nullptr) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(p), n);
}

std::string Reader::ascii_printable(std::uint32_t max_len) {
    std::string s = string(max_len);
    for (const char c : s) {
        const auto b = static_cast<unsigned char>(c);
        if (b < 0x20 || b > 0x7E) {
            fail(DecodeError::InvalidContent);
            return {};
        }
    }
    return s;
}

bool Reader::option_tag() noexcept {
    const std::uint8_t t = u8();
    if (t > 1) {
        fail(DecodeError::InvalidOptionTag);
        return false;
    }
    return t == 1;
}

std::uint8_t Reader::enum_tag() noexcept { return u8(); }

Status<DecodeError> Reader::finish() const noexcept {
    if (error_) {
        return unexpected(*error_);
    }
    if (remaining() != 0) {
        return unexpected(DecodeError::TrailingBytes);
    }
    return Ok{};
}

}  // namespace yap::ycs
