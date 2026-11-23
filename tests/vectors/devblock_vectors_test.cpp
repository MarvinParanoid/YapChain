// SPDX-License-Identifier: Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "vector_util.hpp"
#include "yap/dev/dev_block.hpp"
#include "yap/merkle/merkle.hpp"

using namespace yap;
using test::jhex;
using test::jhexn;
using test::ju64;

TEST_CASE("dev block vectors (protocol §14)", "[vectors][dev]") {
    const auto vec = test::load_vectors("devblock.json");
    for (const auto& c : vec.at("cases")) {
        INFO(c.at("name").get<std::string>());
        const auto producer = crypto::SecretKey::from_seed(jhexn<32>(c.at("producer_seed")));
        dev::DevBlock b;
        b.header.chain_id = c.at("chain_id").get<std::string>();
        b.header.height = ju64(c.at("height"));
        b.header.time_ms = ju64(c.at("time_ms"));
        b.header.prev_block_hash = jhexn<32>(c.at("prev_block_hash"));
        b.header.state_root = jhexn<32>(c.at("state_root"));
        b.header.producer_pubkey = producer.public_key();
        for (const auto& t : c.at("txs")) b.txs.push_back(jhex(t));
        b.header.tx_root = merkle::rfc6962_root(b.txs);
        CHECK(to_hex(b.header.tx_root) == c.at("tx_root").get<std::string>());
        CHECK(to_hex(ycs::encode(b.header)) == c.at("header_ycs").get<std::string>());
        CHECK(to_hex(dev::block_hash(b.header)) == c.at("block_hash").get<std::string>());
        b.signature = dev::sign_header(b.header, producer);
        CHECK(to_hex(b.signature) == c.at("signature").get<std::string>());
        CHECK(to_hex(ycs::encode(b)) == c.at("block_ycs").get<std::string>());
        auto decoded = ycs::decode<dev::DevBlock>(jhex(c.at("block_ycs")));
        REQUIRE(decoded.has_value());
        CHECK(*decoded == b);
        CHECK(dev::verify_header_signature(*decoded));
    }
}
