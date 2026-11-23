// SPDX-License-Identifier: Apache-2.0
//
// Genesis (docs/protocol.md §11, ADR-0016, ADR-0021).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "yap/common/result.hpp"
#include "yap/state/kv.hpp"
#include "yap/state/types.hpp"
#include "yap/tx/params.hpp"

namespace yap::chain {

inline constexpr std::uint32_t kMaxGenesisAccounts = 1'000'000;
inline constexpr std::uint32_t kMaxGenesisValidators = 32;
inline constexpr std::uint32_t kMaxGenesisIssuers = 1'024;
inline constexpr std::uint32_t kMaxGenesisAchievements = 65'536;
inline constexpr std::uint64_t kMaxTotalVotingPower = 1ULL << 60;

struct GenesisAccount {
    crypto::AccountId account_id{};
    std::uint64_t balance = 0;
    bool operator==(const GenesisAccount&) const = default;
};

struct GenesisValidator {
    crypto::PublicKey consensus_pubkey{};
    std::uint64_t power = 0;
    crypto::AccountId reward_account{};
    std::string name;
    bool operator==(const GenesisValidator&) const = default;
};

struct Genesis {
    std::string chain_id;
    std::uint64_t genesis_time_ms = 0;
    std::uint64_t initial_height = 1;
    tx::ChainParams params;
    std::vector<GenesisAccount> accounts;
    std::vector<GenesisValidator> validators;
    std::vector<state::Issuer> issuers;
    std::vector<state::Achievement> achievements;

    bool operator==(const Genesis&) const = default;
    void encode_to(ycs::Writer& w) const;
};

/// genesis_hash = H_tag("YAP/genesis/v1", YCS(Genesis)).
[[nodiscard]] Hash32 genesis_hash(const Genesis& g);

/// §11.3: "yap" for the mainnet chain_id (not defined yet, OPEN), otherwise "tyap".
[[nodiscard]] std::string_view hrp_for_chain(std::string_view chain_id) noexcept;

/// §11.2 rules 1-5. Returns an empty string if valid, otherwise the first violation.
[[nodiscard]] std::string validate_genesis(const Genesis& g);

/// Strict JSON parsing (§11.3 schema) followed by validate_genesis(). The error string names
/// the offending field.
[[nodiscard]] Result<Genesis, std::string> parse_genesis_json(std::string_view text);

/// Writes the initial state of §11.4 item 3 into `out`. `g` must be valid.
void write_initial_state(const Genesis& g, state::Overlay& out);

}  // namespace yap::chain
