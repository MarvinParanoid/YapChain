// SPDX-License-Identifier: Apache-2.0
//
// Registry of hash tags and signature prefixes (docs/protocol.md §4.3, §4.4).
#pragma once

#include <array>
#include <string_view>

namespace yap::crypto {

namespace tag {
inline constexpr std::string_view kAddress = "YAP/addr/v1";
inline constexpr std::string_view kSystemAccount = "YAP/sys-account/v1";
inline constexpr std::string_view kTxId = "YAP/txid/v1";
inline constexpr std::string_view kStateKey = "YAP/state-key/v1";
inline constexpr std::string_view kStateValue = "YAP/state-value/v1";
inline constexpr std::string_view kSmtLeaf = "YAP/smt/leaf/v1";
inline constexpr std::string_view kSmtNode = "YAP/smt/node/v1";
inline constexpr std::string_view kGenesis = "YAP/genesis/v1";
inline constexpr std::string_view kNullifier = "YAP/nullifier/v1";
inline constexpr std::string_view kDevBlock = "YAP/devblock/v1";

inline constexpr std::array kAll{kAddress,   kSystemAccount, kTxId,    kStateKey,  kStateValue,
                                 kSmtLeaf,   kSmtNode,       kGenesis, kNullifier, kDevBlock};
}  // namespace tag

namespace sign_prefix {
using namespace std::string_view_literals;
// The trailing NUL is part of every prefix (prefix-free set).
inline constexpr std::string_view kTx = "YAPCHAIN/TX/V1\0"sv;
inline constexpr std::string_view kAttestation = "YAPCHAIN/ATTESTATION/V1\0"sv;
inline constexpr std::string_view kDevBlock = "YAPCHAIN/DEVBLOCK/V1\0"sv;
}  // namespace sign_prefix

}  // namespace yap::crypto
