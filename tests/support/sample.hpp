// SPDX-License-Identifier: Apache-2.0
//
// Composite YCS test schema shared by vector tests and fuzz targets.
// Mirrors `sample` in tools/refgen/ycs_ref.py.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "yap/codec/ycs.hpp"

namespace yap::test {

struct Sample {
    std::uint16_t version = 0;
    std::string name;  // printable ASCII, max 16
    FixedBytes<4> tag{};
    std::uint64_t amount = 0;
    std::optional<std::uint64_t> maybe;
    std::vector<Bytes> items;  // max 3 elements, each max 4 bytes
    bool flag = false;
    std::optional<std::uint32_t> kind_value;  // enum Kind { 0: Empty, 1: Value{u32} }

    bool operator==(const Sample&) const = default;

    void encode_to(ycs::Writer& w) const {
        w.u16(version);
        w.string(name);
        w.fixed(tag);
        w.u64(amount);
        w.option_tag(maybe.has_value());
        if (maybe) {
            w.u64(*maybe);
        }
        w.length(items.size());
        for (const auto& item : items) {
            w.bytes(item);
        }
        w.boolean(flag);
        if (kind_value) {
            w.enum_tag(1);
            w.u32(*kind_value);
        } else {
            w.enum_tag(0);
        }
    }

    static Sample decode_from(ycs::Reader& r) {
        Sample s;
        s.version = r.u16();
        s.name = r.ascii_printable(16);
        s.tag = r.fixed<4>();
        s.amount = r.u64();
        if (r.option_tag()) {
            s.maybe = r.u64();
        }
        const std::uint32_t n = r.vec_length(3, 4);
        for (std::uint32_t i = 0; i < n && r.ok(); ++i) {
            s.items.push_back(r.bytes(4));
        }
        s.flag = r.boolean();
        switch (r.enum_tag()) {
            case 0: break;
            case 1: s.kind_value = r.u32(); break;
            default: r.fail(ycs::DecodeError::InvalidEnumTag); break;
        }
        return s;
    }
};

}  // namespace yap::test
