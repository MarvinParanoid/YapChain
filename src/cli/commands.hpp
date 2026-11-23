// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "context.hpp"
#include "yap/wallet/keystore.hpp"

namespace CLI {
class App;
}

namespace yap::cli {

void register_keys(CLI::App& app, Context& ctx);

wallet::KeyInfo load_key_info(const std::filesystem::path& path, wallet::KeyPurpose expected);
crypto::SecretKey unlock_key(const Context& ctx, const std::filesystem::path& path, wallet::KeyPurpose expected);
wallet::KeyInfo create_key_file(const Context& ctx, const std::filesystem::path& path, wallet::KeyPurpose purpose);

}  // namespace yap::cli
