// SPDX-License-Identifier: Apache-2.0
//
// YCS — YapChain Canonical Serialization (docs/protocol.md §3).
//
// Writer produces the single canonical encoding. Reader decodes strictly and uses
// a *sticky error*: after the first failure every read is a no-op returning a zero
// value, so decoding code can be written straight-line and checked once at the end
// with finish(). Lengths are validated before any allocation.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "yap/common/bytes.hpp"
#include "yap/common/result.hpp"

namespace yap::ycs {

enum class DecodeError : std::uint8_t {
    Truncated = 1,       ///< input ended before the value was complete
    TrailingBytes,       ///< bytes left after the root value
    InvalidBool,         ///< bool byte other than 0x00 / 0x01
    InvalidOptionTag,    ///< option tag other than 0x00 / 0x01
    InvalidEnumTag,      ///< enum tag not defined by the schema
    LengthLimit,         ///< length / element count above the field limit
    InvalidContent,      ///< field content rule violated (charset, range, ...)
};

[[nodiscard]] std::string_view to_string(DecodeError e) noexcept;

class Writer {
public:
    void u8(std::uint8_t v);
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void boolean(bool v);
    /// Fixed-size array: raw bytes, no length prefix.
    void fixed(ByteView v);
    /// Variable-size byte string: u32 length + bytes. Throws std::length_error above 2^32-1.
    void bytes(ByteView v);
    void string(std::string_view v);
    /// Element count of a vec<T>; the caller then writes the elements.
    void length(std::size_t count);
    void option_tag(bool present);
    void enum_tag(std::uint8_t tag);

    [[nodiscard]] const Bytes& data() const& noexcept { return buf_; }
    [[nodiscard]] Bytes take() && noexcept { return std::move(buf_); }

private:
    Bytes buf_;
};

class Reader {
public:
    explicit Reader(ByteView input) noexcept : in_(input) {}

    std::uint8_t u8() noexcept;
    std::uint16_t u16() noexcept;
    std::uint32_t u32() noexcept;
    std::uint64_t u64() noexcept;
    bool boolean() noexcept;

    template <std::size_t N>
    FixedBytes<N> fixed() noexcept {
        FixedBytes<N> out{};
        if (const auto* p = consume(N)) {
            for (std::size_t i = 0; i < N; ++i) {
                out[i] = p[i];
            }
        }
        return out;
    }

    /// bytes with at most `max_len` bytes.
    Bytes bytes(std::uint32_t max_len);
    /// string with at most `max_len` bytes; content rules are the caller's job (see fail()).
    std::string string(std::uint32_t max_len);
    /// string restricted to printable ASCII 0x20..0x7E.
    std::string ascii_printable(std::uint32_t max_len);
    /// Element count of a vec<T>: count <= max_count and count * min_element_size <= remaining().
    std::uint32_t vec_length(std::uint32_t max_count, std::size_t min_element_size) noexcept;
    bool option_tag() noexcept;
    /// Raw enum tag; the caller must call fail(InvalidEnumTag) for undefined tags.
    std::uint8_t enum_tag() noexcept;

    /// Record an error (the first error wins).
    void fail(DecodeError e) noexcept;

    [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
    [[nodiscard]] std::optional<DecodeError> error() const noexcept { return error_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return in_.size() - pos_; }

    /// Ends decoding of a root value: reports the sticky error or TrailingBytes.
    [[nodiscard]] Status<DecodeError> finish() const noexcept;

private:
    /// Returns a pointer to n bytes and advances, or nullptr (and records Truncated).
    const Byte* consume(std::size_t n) noexcept;
    std::uint64_t uint_le(std::size_t width) noexcept;
    std::uint32_t byte_length(std::uint32_t max_len) noexcept;

    ByteView in_;
    std::size_t pos_ = 0;
    std::optional<DecodeError> error_;
};

/// Decode a root value. T must provide `static T decode_from(Reader&)`.
template <class T>
[[nodiscard]] Result<T, DecodeError> decode(ByteView input) {
    Reader r(input);
    T value = T::decode_from(r);
    auto st = r.finish();
    if (!st) {
        return unexpected(st.error());
    }
    return value;
}

/// Encode a value. T must provide `void encode_to(Writer&) const`.
template <class T>
[[nodiscard]] Bytes encode(const T& value) {
    Writer w;
    value.encode_to(w);
    return std::move(w).take();
}

}  // namespace yap::ycs
