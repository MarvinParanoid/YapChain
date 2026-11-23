// SPDX-License-Identifier: Apache-2.0
#include "yap/chain/genesis.hpp"

#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>

#include "yap/common/checked.hpp"
#include "yap/common/hex.hpp"
#include "yap/crypto/domains.hpp"
#include "yap/crypto/hash.hpp"
#include "yap/state/economics.hpp"
#include "yap/state/state.hpp"
#include "yap/tx/chain_id.hpp"

namespace yap::chain {

using nlohmann::json;

// ------------------------------------------------------------------ YCS and hash

void Genesis::encode_to(ycs::Writer& w) const {
    w.string(chain_id);
    w.u64(genesis_time_ms);
    w.u64(initial_height);
    params.encode_to(w);
    w.length(accounts.size());
    for (const auto& a : accounts) {
        w.fixed(a.account_id);
        w.u64(a.balance);
    }
    w.length(validators.size());
    for (const auto& v : validators) {
        w.fixed(v.consensus_pubkey);
        w.u64(v.power);
        w.fixed(v.reward_account);
        w.string(v.name);
    }
    w.length(issuers.size());
    for (const auto& i : issuers) i.encode_to(w);
    w.length(achievements.size());
    for (const auto& a : achievements) a.encode_to(w);
}

Hash32 genesis_hash(const Genesis& g) { return crypto::tagged_hash(crypto::tag::kGenesis, ycs::encode(g)); }

std::string_view hrp_for_chain(std::string_view /*chain_id*/) noexcept {
    // The mainnet chain_id is OPEN (protocol.md §2); until it is defined every chain is a test network.
    return crypto::kTestnetHrp;
}

// ------------------------------------------------------------------ validation (§11.2)

namespace {

bool valid_name(std::string_view s) {
    if (s.empty() || s.size() > state::kMaxNameBytes) return false;
    for (const char c : s) {
        const auto b = static_cast<unsigned char>(c);
        if (b < 0x20 || b > 0x7E) return false;
    }
    return true;
}

bool valid_status(std::uint8_t s) { return s == state::kStatusActive || s == state::kStatusInactive; }

}  // namespace

std::string validate_genesis(const Genesis& g) {
    // 1
    if (!tx::is_valid_chain_id(g.chain_id)) return "chain_id: invalid";
    if (g.initial_height != 1) return "initial_height: must be 1";
    // 2
    if (auto e = tx::check_params(g.params); !e.empty()) return "params: " + std::string(e);
    // 3
    if (g.accounts.size() > kMaxGenesisAccounts) return "accounts: too many";
    std::uint64_t supply = 0;
    for (std::size_t i = 0; i < g.accounts.size(); ++i) {
        const auto& a = g.accounts[i];
        if (i > 0 && !(g.accounts[i - 1].account_id < a.account_id)) return "accounts: not strictly ascending";
        if (a.balance < 1) return "accounts: balance must be >= 1";
        const auto s = checked_add(supply, a.balance);
        if (!s) return "accounts: sum of balances overflows";
        supply = *s;
    }
    if (supply > g.params.max_issuance) return "accounts: sum of balances exceeds max_issuance";
    // 4
    if (g.validators.empty() || g.validators.size() > kMaxGenesisValidators) return "validators: need 1..32";
    std::uint64_t power = 0;
    for (std::size_t i = 0; i < g.validators.size(); ++i) {
        const auto& v = g.validators[i];
        if (!crypto::is_valid_validator_public_key(v.consensus_pubkey)) {
            return "validators: consensus_pubkey fails protocol §5.7";
        }
        if (i > 0 && !(state::consensus_address(g.validators[i - 1].consensus_pubkey) <
                       state::consensus_address(v.consensus_pubkey))) {
            return "validators: not strictly ascending by consensus address";
        }
        if (v.power < 1) return "validators: power must be >= 1";
        const auto p = checked_add(power, v.power);
        if (!p || *p > kMaxTotalVotingPower) return "validators: total power exceeds 2^60";
        power = *p;
        if (!valid_name(v.name)) return "validators: invalid name";
    }
    // 5
    if (g.issuers.size() > kMaxGenesisIssuers) return "issuers: too many";
    std::set<std::uint32_t> issuer_ids;
    for (std::size_t i = 0; i < g.issuers.size(); ++i) {
        const auto& is = g.issuers[i];
        if (i > 0 && !(g.issuers[i - 1].issuer_id < is.issuer_id)) return "issuers: not strictly ascending";
        if (!valid_name(is.name)) return "issuers: invalid name";
        if (!valid_status(is.status)) return "issuers: invalid status";
        if (is.keys.empty() || is.keys.size() > state::kMaxIssuerKeys) return "issuers: need 1..8 keys";
        for (std::size_t k = 0; k < is.keys.size(); ++k) {
            if (k > 0 && !(is.keys[k - 1].key_id < is.keys[k].key_id)) return "issuers: key_id not ascending";
            if (!valid_status(is.keys[k].status)) return "issuers: invalid key status";
        }
        issuer_ids.insert(is.issuer_id);
    }
    if (g.achievements.size() > kMaxGenesisAchievements) return "achievements: too many";
    for (std::size_t i = 0; i < g.achievements.size(); ++i) {
        const auto& a = g.achievements[i];
        if (i > 0) {
            const auto& p = g.achievements[i - 1];
            if (!(std::pair(p.issuer_id, p.achievement_id) < std::pair(a.issuer_id, a.achievement_id))) {
                return "achievements: not strictly ascending";
            }
        }
        if (!issuer_ids.contains(a.issuer_id)) return "achievements: unknown issuer_id";
        if (a.reward < 1) return "achievements: reward must be >= 1";
        if (a.policy == state::kPolicyOnce) {
            if (a.max_per_epoch != 0) return "achievements: Once requires max_per_epoch = 0";
        } else if (a.policy == state::kPolicyPerEpoch) {
            if (a.max_per_epoch < 1) return "achievements: PerEpoch requires max_per_epoch >= 1";
        } else {
            return "achievements: invalid policy";
        }
        if (!valid_status(a.status)) return "achievements: invalid status";
    }
    return {};
}

// ------------------------------------------------------------------ strict JSON (§11.3)

namespace {

struct JsonError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& path, const std::string& what) { throw JsonError(path + ": " + what); }

const json& field(const json& obj, const char* name, const std::string& path) {
    auto it = obj.find(name);
    if (it == obj.end()) fail(path + "." + name, "missing");
    return *it;
}

void exact_fields(const json& obj, std::initializer_list<const char*> names, const std::string& path) {
    if (!obj.is_object()) fail(path, "must be an object");
    std::set<std::string> expected(names.begin(), names.end());
    for (const auto& [k, _] : obj.items()) {
        if (!expected.contains(k)) fail(path + "." + k, "unknown field");
    }
    for (const auto* n : names) (void)field(obj, n, path);
}

std::uint64_t parse_u64(const json& j, const std::string& path) {
    if (!j.is_string()) fail(path, "u64 must be a decimal string");
    const auto& s = j.get_ref<const std::string&>();
    if (s.empty() || s.size() > 20 || (s.size() > 1 && s[0] == '0')) fail(path, "invalid u64 string");
    std::uint64_t v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') fail(path, "invalid u64 string");
        const auto times10 = checked_mul(v, 10);
        if (!times10) fail(path, "u64 out of range");
        const auto next = checked_add(*times10, static_cast<std::uint64_t>(c - '0'));
        if (!next) fail(path, "u64 out of range");
        v = *next;
    }
    return v;
}

template <class T>
T parse_small(const json& j, const std::string& path) {
    // Non-negative JSON integers are stored as number_unsigned; fractions/exponents become floats.
    if (!j.is_number_unsigned()) fail(path, "must be a non-negative JSON integer");
    const auto v = j.get<std::uint64_t>();
    if (v > std::numeric_limits<T>::max()) fail(path, "out of range");
    return static_cast<T>(v);
}

std::string parse_str(const json& j, const std::string& path) {
    if (!j.is_string()) fail(path, "must be a string");
    return j.get<std::string>();
}

crypto::AccountId parse_addr(const json& j, std::string_view hrp, const std::string& path) {
    auto id = crypto::decode_address(parse_str(j, path), hrp);
    if (!id) fail(path, "invalid address (" + std::string(crypto::to_string(id.error())) + ")");
    return *id;
}

crypto::PublicKey parse_hex32(const json& j, const std::string& path) {
    auto b = from_hex_fixed<32>(parse_str(j, path));
    if (!b) fail(path, "must be 64 lowercase hex characters");
    return *b;
}

const json& array(const json& obj, const char* name, const std::string& path) {
    const json& a = field(obj, name, path);
    if (!a.is_array()) fail(path + "." + name, "must be an array");
    return a;
}

std::string idx(const std::string& path, std::size_t i) { return path + "[" + std::to_string(i) + "]"; }

Genesis from_json(const json& root) {
    const std::string top = "$";
    exact_fields(root,
                 {"chain_id", "genesis_time_ms", "initial_height", "params", "accounts", "validators", "issuers",
                  "achievements"},
                 "$");
    Genesis g;
    g.chain_id = parse_str(root.at("chain_id"), "$.chain_id");
    if (!tx::is_valid_chain_id(g.chain_id)) fail("$.chain_id", "invalid chain_id");
    const std::string_view hrp = hrp_for_chain(g.chain_id);
    g.genesis_time_ms = parse_u64(root.at("genesis_time_ms"), "$.genesis_time_ms");
    g.initial_height = parse_u64(root.at("initial_height"), "$.initial_height");

    const json& p = root.at("params");
    exact_fields(p,
                 {"min_fee", "fee_burn_bps", "block_reward", "reward_validators_bps", "reward_pool_bps",
                  "reward_treasury_bps", "max_issuance", "max_tx_bytes", "max_memo_bytes", "max_tx_ttl_blocks",
                  "epoch_length_blocks", "max_attestation_validity_s", "attestation_clock_skew_s",
                  "max_claims_per_subject_per_epoch"},
                 "$.params");
    auto& gp = g.params;
    gp.min_fee = parse_u64(p.at("min_fee"), "$.params.min_fee");
    gp.fee_burn_bps = parse_small<std::uint16_t>(p.at("fee_burn_bps"), "$.params.fee_burn_bps");
    gp.block_reward = parse_u64(p.at("block_reward"), "$.params.block_reward");
    gp.reward_validators_bps = parse_small<std::uint16_t>(p.at("reward_validators_bps"), "$.params.reward_validators_bps");
    gp.reward_pool_bps = parse_small<std::uint16_t>(p.at("reward_pool_bps"), "$.params.reward_pool_bps");
    gp.reward_treasury_bps = parse_small<std::uint16_t>(p.at("reward_treasury_bps"), "$.params.reward_treasury_bps");
    gp.max_issuance = parse_u64(p.at("max_issuance"), "$.params.max_issuance");
    gp.max_tx_bytes = parse_small<std::uint32_t>(p.at("max_tx_bytes"), "$.params.max_tx_bytes");
    gp.max_memo_bytes = parse_small<std::uint32_t>(p.at("max_memo_bytes"), "$.params.max_memo_bytes");
    gp.max_tx_ttl_blocks = parse_u64(p.at("max_tx_ttl_blocks"), "$.params.max_tx_ttl_blocks");
    gp.epoch_length_blocks = parse_u64(p.at("epoch_length_blocks"), "$.params.epoch_length_blocks");
    gp.max_attestation_validity_s = parse_u64(p.at("max_attestation_validity_s"), "$.params.max_attestation_validity_s");
    gp.attestation_clock_skew_s = parse_u64(p.at("attestation_clock_skew_s"), "$.params.attestation_clock_skew_s");
    gp.max_claims_per_subject_per_epoch =
        parse_small<std::uint16_t>(p.at("max_claims_per_subject_per_epoch"), "$.params.max_claims_per_subject_per_epoch");

    const json& accounts = array(root, "accounts", top);
    if (accounts.size() > kMaxGenesisAccounts) fail("$.accounts", "too many entries");
    for (std::size_t i = 0; i < accounts.size(); ++i) {
        const auto path = idx("$.accounts", i);
        exact_fields(accounts[i], {"address", "balance"}, path);
        g.accounts.push_back({parse_addr(accounts[i].at("address"), hrp, path + ".address"),
                              parse_u64(accounts[i].at("balance"), path + ".balance")});
    }

    const json& validators = array(root, "validators", top);
    if (validators.size() > kMaxGenesisValidators) fail("$.validators", "too many entries");
    for (std::size_t i = 0; i < validators.size(); ++i) {
        const auto path = idx("$.validators", i);
        const json& v = validators[i];
        exact_fields(v, {"consensus_pubkey", "power", "reward_account", "name"}, path);
        g.validators.push_back({parse_hex32(v.at("consensus_pubkey"), path + ".consensus_pubkey"),
                                parse_u64(v.at("power"), path + ".power"),
                                parse_addr(v.at("reward_account"), hrp, path + ".reward_account"),
                                parse_str(v.at("name"), path + ".name")});
    }

    const json& issuers = array(root, "issuers", top);
    if (issuers.size() > kMaxGenesisIssuers) fail("$.issuers", "too many entries");
    for (std::size_t i = 0; i < issuers.size(); ++i) {
        const auto path = idx("$.issuers", i);
        const json& is = issuers[i];
        exact_fields(is, {"issuer_id", "name", "status", "epoch_budget", "keys"}, path);
        state::Issuer out;
        out.issuer_id = parse_small<std::uint32_t>(is.at("issuer_id"), path + ".issuer_id");
        out.name = parse_str(is.at("name"), path + ".name");
        out.status = parse_small<std::uint8_t>(is.at("status"), path + ".status");
        out.epoch_budget = parse_u64(is.at("epoch_budget"), path + ".epoch_budget");
        const json& keys = array(is, "keys", path);  // `path` is a named variable
        if (keys.size() > state::kMaxIssuerKeys) fail(path + ".keys", "too many entries");
        for (std::size_t k = 0; k < keys.size(); ++k) {
            const auto kp = idx(path + ".keys", k);
            exact_fields(keys[k], {"key_id", "pubkey", "status"}, kp);
            out.keys.push_back({parse_small<std::uint8_t>(keys[k].at("key_id"), kp + ".key_id"),
                                parse_hex32(keys[k].at("pubkey"), kp + ".pubkey"),
                                parse_small<std::uint8_t>(keys[k].at("status"), kp + ".status")});
        }
        g.issuers.push_back(std::move(out));
    }

    const json& achievements = array(root, "achievements", top);
    if (achievements.size() > kMaxGenesisAchievements) fail("$.achievements", "too many entries");
    for (std::size_t i = 0; i < achievements.size(); ++i) {
        const auto path = idx("$.achievements", i);
        const json& a = achievements[i];
        exact_fields(a, {"issuer_id", "achievement_id", "reward", "policy", "max_per_epoch", "status"}, path);
        g.achievements.push_back({parse_small<std::uint32_t>(a.at("issuer_id"), path + ".issuer_id"),
                                  parse_small<std::uint32_t>(a.at("achievement_id"), path + ".achievement_id"),
                                  parse_u64(a.at("reward"), path + ".reward"),
                                  parse_small<std::uint8_t>(a.at("policy"), path + ".policy"),
                                  parse_small<std::uint16_t>(a.at("max_per_epoch"), path + ".max_per_epoch"),
                                  parse_small<std::uint8_t>(a.at("status"), path + ".status")});
    }
    return g;
}

}  // namespace

Result<Genesis, std::string> parse_genesis_json(std::string_view text) {
    // Duplicate keys are detected during parsing (nlohmann keeps the last one otherwise).
    std::vector<std::set<std::string>> scopes;
    bool duplicate = false;
    const json::parser_callback_t cb = [&](int /*depth*/, json::parse_event_t event, json& parsed) {
        switch (event) {
            case json::parse_event_t::object_start: scopes.emplace_back(); break;
            case json::parse_event_t::object_end:
                if (!scopes.empty()) scopes.pop_back();
                break;
            case json::parse_event_t::key:
                if (!scopes.empty() && !scopes.back().insert(parsed.get<std::string>()).second) duplicate = true;
                break;
            default: break;
        }
        return true;
    };
    const json root = json::parse(text.begin(), text.end(), cb, /*allow_exceptions=*/false);
    if (root.is_discarded()) return unexpected(std::string("$: malformed JSON"));
    if (duplicate) return unexpected(std::string("$: duplicate object key"));
    Genesis g;
    try {
        g = from_json(root);
    } catch (const JsonError& e) {
        return unexpected(std::string(e.what()));
    }
    if (auto err = validate_genesis(g); !err.empty()) return unexpected(std::move(err));
    return g;
}

// ------------------------------------------------------------------ initial state (§11.4)

void write_initial_state(const Genesis& g, state::Overlay& out) {
    state::State st(out);
    st.put_params(g.params);
    std::vector<std::uint64_t> balances;
    balances.reserve(g.accounts.size());
    for (const auto& a : g.accounts) {
        st.put_account(a.account_id, {a.balance, 0});
        balances.push_back(a.balance);
    }
    for (const auto& v : g.validators) {
        st.put_validator(state::consensus_address(v.consensus_pubkey),
                         {v.name, v.consensus_pubkey, v.power, v.reward_account});
    }
    for (const auto& is : g.issuers) out.put(state::key::issuer(is.issuer_id), ycs::encode(is));
    for (const auto& a : g.achievements) {
        out.put(state::key::achievement(a.issuer_id, a.achievement_id), ycs::encode(a));
    }
    const auto cs = state::genesis_chain_state(balances, g.genesis_time_ms, g.params.max_issuance);
    if (!cs) throw FailStop("genesis supply invalid (validate_genesis was not called?)");
    st.put_chain_state(*cs);
}

}  // namespace yap::chain
