// SPDX-License-Identifier: Apache-2.0
// yap keys create|list|show, yap addr validate
#include <CLI11.hpp>

#include <algorithm>
#include <memory>

#include "commands.hpp"
#include "yap/common/hex.hpp"
#include "yap/wallet/keystore.hpp"

namespace yap::cli {

wallet::KeyInfo load_key_info(const std::filesystem::path& path, wallet::KeyPurpose expected) {
    std::string json;
    try {
        json = wallet::read_private_file(path);
    } catch (const std::exception& e) {
        throw CliError(kExitAuth, e.what());
    }
    auto info = wallet::read_key_info(json);
    if (!info) throw CliError(kExitAuth, path.string() + ": " + std::string(wallet::to_string(info.error())));
    if (info->purpose != expected) {
        throw CliError(kExitAuth, path.string() + ": " + std::string(wallet::to_string(wallet::KeystoreError::WrongPurpose)));
    }
    return *info;
}

crypto::SecretKey unlock_key(const Context& ctx, const std::filesystem::path& path, wallet::KeyPurpose expected) {
    std::string json;
    try {
        json = wallet::read_private_file(path);
    } catch (const std::exception& e) {
        throw CliError(kExitAuth, e.what());
    }
    const auto pw = read_password(ctx, "Password for " + path.filename().string() + ": ", false);
    auto key = wallet::unlock_keystore(json, pw, expected);
    if (!key) throw CliError(kExitAuth, path.string() + ": " + std::string(wallet::to_string(key.error())));
    return std::move(key).value();
}

wallet::KeyInfo create_key_file(const Context& ctx, const std::filesystem::path& path, wallet::KeyPurpose purpose) {
    if (std::filesystem::exists(path)) throw CliError(kExitRejected, path.string() + " already exists");
    const auto pw = read_password(ctx, "New password: ", true);
    if (pw.size() < 8) throw CliError(kExitAuth, "password must have at least 8 characters");
    const auto k = wallet::create_keystore(pw, purpose, wallet::KdfParams::moderate());
    try {
        wallet::write_private_file(path, k.json);
    } catch (const std::exception& e) {
        throw CliError(kExitStorage, e.what());
    }
    return k.info;
}

void register_keys(CLI::App& app, Context& ctx) {
    auto* keys = app.add_subcommand("keys", "Manage account keys (encrypted keystores)");
    keys->require_subcommand(1);

    auto name = std::make_shared<std::string>();
    auto* create = keys->add_subcommand("create", "Create a new account key from the OS CSPRNG");
    create->add_option("name", *name, "Key name [a-z0-9_-]{1,32}")->required();
    create->callback([&ctx, name] {
        const auto info = create_key_file(ctx, ctx.key_path(*name), wallet::KeyPurpose::Account);
        out("name", *name);
        out("address", info.address);
        out("public_key", to_hex(info.public_key));
    });

    auto* list = keys->add_subcommand("list", "List account keys");
    list->callback([&ctx] {
        std::vector<std::filesystem::path> files;
        if (std::filesystem::is_directory(ctx.keys_dir())) {
            for (const auto& e : std::filesystem::directory_iterator(ctx.keys_dir())) {
                if (e.path().extension() == ".json") files.push_back(e.path());
            }
        }
        std::sort(files.begin(), files.end());
        bool bad = false;
        for (const auto& f : files) {
            try {
                const auto info = load_key_info(f, wallet::KeyPurpose::Account);
                std::cout << f.stem().string() << " " << info.address << "\n";
            } catch (const CliError& e) {
                std::cerr << "error: " << e.what() << "\n";  // report and keep listing the others
                bad = true;
            }
        }
        if (bad) throw CliError(kExitAuth, "some key files could not be read");
    });

    auto show_name = std::make_shared<std::string>();
    auto* show = keys->add_subcommand("show", "Show the public part of an account key (never the secret)");
    show->add_option("name", *show_name, "Key name")->required();
    show->callback([&ctx, show_name] {
        const auto info = load_key_info(ctx.key_path(*show_name), wallet::KeyPurpose::Account);
        out("name", *show_name);
        out("purpose", wallet::to_string(info.purpose));
        out("address", info.address);
        out("public_key", to_hex(info.public_key));
        out("account_id", to_hex(info.account_id));
    });

    auto* addr = app.add_subcommand("addr", "Address utilities");
    addr->require_subcommand(1);
    auto text = std::make_shared<std::string>();
    auto* validate = addr->add_subcommand("validate", "Validate a YapChain address (Bech32m, protocol §5.3)");
    validate->add_option("address", *text, "Address to check")->required();
    validate->callback([text] {
        auto id = crypto::decode_address(*text, crypto::kTestnetHrp);
        if (!id) throw CliError(kExitRejected, "invalid address: " + std::string(crypto::to_string(id.error())));
        out("valid", "yes");
        out("address", crypto::encode_address(crypto::kTestnetHrp, *id));
        out("account_id", to_hex(*id));
    });
}

}  // namespace yap::cli
