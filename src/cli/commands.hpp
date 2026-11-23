// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "context.hpp"
#include "yap/chain/genesis.hpp"
#include "yap/wallet/keystore.hpp"

namespace CLI {
class App;
}

namespace yap::cli {

void register_keys(CLI::App& app, Context& ctx);
void register_tx(CLI::App& app, Context& ctx);
void register_genesis(CLI::App& app, Context& ctx);
void register_dev(CLI::App& app, Context& ctx);
void register_query(CLI::App& app, Context& ctx);

chain::Genesis load_genesis(const std::filesystem::path& path);

wallet::KeyInfo load_key_info(const std::filesystem::path& path, wallet::KeyPurpose expected);
crypto::SecretKey unlock_key(const Context& ctx, const std::filesystem::path& path, wallet::KeyPurpose expected);
wallet::KeyInfo create_key_file(const Context& ctx, const std::filesystem::path& path, wallet::KeyPurpose purpose);

}  // namespace yap::cli
