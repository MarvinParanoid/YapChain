// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "fake_state.hpp"
#include "yap/common/checked.hpp"
#include "yap/tx/stateless.hpp"

using namespace yap;
using tx::TxCode;

namespace {
const std::string kChain = "yapchain-testnet-1";

crypto::SecretKey key(Byte b) {
    crypto::Seed s{};
    s.fill(b);
    return crypto::SecretKey::from_seed(s);
}

tx::TxBody body(const crypto::SecretKey& k, std::uint64_t nonce = 0) {
    tx::TxBody b;
    b.chain_id = kChain;
    b.sender_pubkey = k.public_key();
    b.nonce = nonce;
    b.fee = 1000;
    b.expires_at_height = 5;
    b.payload = tx::Transfer{crypto::system_account_id("treasury"), 10};
    return b;
}

Bytes bytes(const tx::SignedTx& t) { return ycs::encode(t); }
}  // namespace

TEST_CASE("stateless: each code is reachable", "[validation]") {
    const auto k = key(1);
    const auto p = tx::testnet_params();
    CHECK(tx::check_stateless(bytes(tx::sign_tx(body(k), k)), kChain, p).code == TxCode::Ok);
    auto b = body(k);
    b.fee = 0;
    const auto r = tx::check_stateless(bytes(tx::sign_tx(b, k)), kChain, p);
    CHECK(r.code == TxCode::ErrFee);
    CHECK(r.tx.has_value());  // decoded even though a later rule failed
    CHECK_FALSE(tx::check_stateless(Bytes{0}, kChain, p).tx.has_value());
}

TEST_CASE("stateful: nonce, expiry, TTL and funds", "[validation]") {
    const auto k = key(2);
    test::FakeState st;
    const auto id = crypto::account_id_from_public_key(k.public_key());
    st.accounts[id] = {5000, 7};

    auto check = [&](const tx::TxBody& b, std::uint64_t h) { return state::check_stateful(tx::sign_tx(b, k), h, st); };
    CHECK(check(body(k, 7), 5) == TxCode::Ok);
    CHECK(check(body(k, 6), 5) == TxCode::ErrNonce);
    CHECK(check(body(k, 8), 5) == TxCode::ErrNonce);
    CHECK(check(body(k, 7), 6) == TxCode::ErrExpired);

    auto far = body(k, 7);
    far.expires_at_height = kU64Max;
    CHECK(check(far, 1) == TxCode::ErrTtlTooLong);  // no overflow in H + ttl
    far.expires_at_height = 1 + st.p.max_tx_ttl_blocks;
    CHECK(check(far, 1) == TxCode::Ok);

    auto rich = body(k, 7);
    rich.payload = tx::Burn{4000};
    CHECK(check(rich, 1) == TxCode::Ok);  // 4000 + 1000 == balance
    rich.payload = tx::Burn{4001};
    CHECK(check(rich, 1) == TxCode::ErrInsufficientFunds);
    rich.payload = tx::Burn{kU64Max};
    CHECK(check(rich, 1) == TxCode::ErrOverflow);
}

TEST_CASE("stateful: nonce u64::MAX cannot be incremented (T4)", "[validation]") {
    const auto k = key(3);
    test::FakeState st;
    st.accounts[crypto::account_id_from_public_key(k.public_key())] = {1'000'000, kU64Max};
    CHECK(state::check_stateful(tx::sign_tx(body(k, kU64Max), k), 1, st) == TxCode::ErrOverflow);
}

TEST_CASE("validate_tx: stateless rules win over stateful ones", "[validation]") {
    const auto k = key(4);
    test::FakeState st;  // account does not exist: nonce 0, balance 0
    auto b = body(k, 9);  // wrong nonce and no funds ...
    b.chain_id = "other";  // ... but the chain id is checked first
    CHECK(state::validate_tx(bytes(tx::sign_tx(b, k)), 1, kChain, st).code == TxCode::ErrChainId);
    b.chain_id = kChain;
    CHECK(state::validate_tx(bytes(tx::sign_tx(b, k)), 1, kChain, st).code == TxCode::ErrNonce);
    b.nonce = 0;
    CHECK(state::validate_tx(bytes(tx::sign_tx(b, k)), 1, kChain, st).code == TxCode::ErrInsufficientFunds);
}

TEST_CASE("result code values are stable (protocol §13)", "[validation]") {
    CHECK(static_cast<std::uint32_t>(TxCode::ErrUnsupported) == 14);
    CHECK(static_cast<std::uint32_t>(TxCode::ErrClaimTime) == 27);
    CHECK(tx::to_string(TxCode::ErrTtlTooLong) == "ERR_TTL_TOO_LONG");
}

TEST_CASE("ChainParams: testnet values are valid and the rules reject bad ones", "[validation][params]") {
    auto p = tx::testnet_params();
    CHECK(tx::check_params(p).empty());
    CHECK(ycs::decode<tx::ChainParams>(ycs::encode(p)).value() == p);
    auto bad = p;
    bad.reward_pool_bps = 2999;
    CHECK_FALSE(tx::check_params(bad).empty());
    bad = p;
    bad.max_tx_bytes = 255;
    CHECK_FALSE(tx::check_params(bad).empty());
    bad = p;
    bad.max_memo_bytes = 1025;
    CHECK_FALSE(tx::check_params(bad).empty());
    bad = p;
    bad.min_fee = 0;
    CHECK_FALSE(tx::check_params(bad).empty());
    bad = p;
    bad.fee_burn_bps = 10001;
    CHECK_FALSE(tx::check_params(bad).empty());
}
