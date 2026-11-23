// SPDX-License-Identifier: Apache-2.0
#include "yap/crypto/address.hpp"

#include "yap/crypto/bech32m.hpp"
#include "yap/crypto/domains.hpp"
#include "yap/crypto/hash.hpp"

namespace yap::crypto {
namespace {
// 1 version symbol + 52 symbols carrying 256 bits plus 4 zero padding bits.
constexpr std::size_t kDataSymbols = 53;
constexpr std::size_t kPayloadSymbols = kDataSymbols - 1;
}  // namespace

AccountId account_id_from_public_key(const PublicKey& public_key) {
    const Byte algorithm = kAlgorithmEd25519;
    return TaggedHasher(tag::kAddress).update(ByteView(&algorithm, 1)).update(public_key).finalize();
}

AccountId system_account_id(std::string_view name) { return tagged_hash(tag::kSystemAccount, as_bytes(name)); }

std::string_view to_string(AddressError e) noexcept {
    switch (e) {
        case AddressError::Bech32: return "bech32";
        case AddressError::WrongHrp: return "wrong_hrp";
        case AddressError::WrongVersion: return "wrong_version";
        case AddressError::WrongLength: return "wrong_length";
        case AddressError::NonZeroPadding: return "non_zero_padding";
    }
    return "unknown";
}

std::string encode_address(std::string_view hrp, const AccountId& account_id) {
    const std::vector<std::uint8_t> bytes(account_id.begin(), account_id.end());
    auto five = convert_bits(bytes, 8, 5, true);  // cannot fail for 8-bit input
    std::vector<std::uint8_t> data;
    data.reserve(kDataSymbols);
    data.push_back(kAddressVersion);
    data.insert(data.end(), five->begin(), five->end());
    return bech32m_encode(hrp, data);
}

Result<AccountId, AddressError> decode_address(std::string_view address, std::string_view expected_hrp) {
    auto decoded = bech32m_decode(address);
    if (!decoded) {
        return unexpected(AddressError::Bech32);
    }
    if (decoded->hrp != expected_hrp) {
        return unexpected(AddressError::WrongHrp);
    }
    const auto& data = decoded->data;
    if (data.empty()) {
        return unexpected(AddressError::WrongLength);
    }
    if (data[0] != kAddressVersion) {
        return unexpected(AddressError::WrongVersion);
    }
    if (data.size() != kDataSymbols) {
        return unexpected(AddressError::WrongLength);
    }
    // 52 symbols * 5 bits = 260 bits: 256 payload bits followed by 4 padding bits that must be zero.
    if ((data.back() & 0x0FU) != 0) {
        return unexpected(AddressError::NonZeroPadding);
    }
    const std::vector<std::uint8_t> payload(data.begin() + 1, data.end());
    static_assert(kPayloadSymbols * 5 == 256 + 4);
    auto bytes = convert_bits(payload, 5, 8, false);
    if (!bytes || bytes->size() != 32) {
        return unexpected(AddressError::WrongLength);  // unreachable after the checks above
    }
    AccountId id{};
    for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = (*bytes)[i];
    }
    return id;
}

}  // namespace yap::crypto
