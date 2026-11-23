// SPDX-License-Identifier: Apache-2.0
#include "yap/dev/dev_block.hpp"

#include "yap/crypto/domains.hpp"
#include "yap/crypto/hash.hpp"
#include "yap/tx/transaction.hpp"

namespace yap::dev {

void DevBlockHeader::encode_to(ycs::Writer& w) const {
    w.u16(version);
    w.string(chain_id);
    w.u64(height);
    w.u64(time_ms);
    w.fixed(prev_block_hash);
    w.fixed(tx_root);
    w.fixed(state_root);
    w.fixed(producer_pubkey);
}

DevBlockHeader DevBlockHeader::decode_from(ycs::Reader& r) {
    DevBlockHeader h;
    h.version = r.u16();
    h.chain_id = tx::read_chain_id(r);
    h.height = r.u64();
    h.time_ms = r.u64();
    h.prev_block_hash = r.fixed<32>();
    h.tx_root = r.fixed<32>();
    h.state_root = r.fixed<32>();
    h.producer_pubkey = r.fixed<32>();
    return h;
}

void DevBlock::encode_to(ycs::Writer& w) const {
    header.encode_to(w);
    w.length(txs.size());
    for (const auto& t : txs) w.bytes(t);
    w.fixed(signature);
}

DevBlock DevBlock::decode_from(ycs::Reader& r) {
    DevBlock b;
    b.header = DevBlockHeader::decode_from(r);
    const std::uint32_t n = r.vec_length(kMaxDevBlockTxs, 4);
    b.txs.reserve(n);
    for (std::uint32_t i = 0; i < n && r.ok(); ++i) b.txs.push_back(r.bytes(kMaxDevBlockTxBytes));
    b.signature = r.fixed<64>();
    return b;
}

Hash32 block_hash(const DevBlockHeader& header) {
    return crypto::tagged_hash(crypto::tag::kDevBlock, ycs::encode(header));
}

crypto::Signature sign_header(const DevBlockHeader& header, const crypto::SecretKey& producer) {
    return producer.sign_prefixed(crypto::sign_prefix::kDevBlock, ycs::encode(header));
}

bool verify_header_signature(const DevBlock& block) {
    return crypto::verify_prefixed(block.header.producer_pubkey, crypto::sign_prefix::kDevBlock,
                                   ycs::encode(block.header), block.signature);
}

}  // namespace yap::dev
