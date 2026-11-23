// SPDX-License-Identifier: Apache-2.0
//
// yap — YapChain command-line tool (M1: local wallet, transactions and dev-chain; no network).
#include <CLI11.hpp>

#include <iostream>

#include "commands.hpp"
#include "yap/chain/store_layout.hpp"
#include "yap/common/fail_stop.hpp"
#include "yap/dev/dev_chain.hpp"
#include "yap/storage/store.hpp"

int main(int argc, char** argv) {
    using namespace yap::cli;
    CLI::App app{"yap — YapChain local tool. The economy is real, the value is questionable.\n"
                 "Exit codes: 0 ok, 1 usage, 2 rejected/invalid, 3 storage, 4 authentication, 5 internal."};
    app.require_subcommand(1);
    Context ctx;
    std::string home;
    std::string password_file;
    app.add_option("--home", home, "Data directory (default: $YAP_HOME or ~/.yapchain)");
    app.add_option("--password-file", password_file,
                   "Read the password from this file (mode 0600) instead of the terminal/stdin");
    app.parse_complete_callback([&] {
        ctx.home = home.empty() ? default_home() : std::filesystem::path(home);
        ctx.password_file = password_file;
    });
    register_keys(app, ctx);
    register_tx(app, ctx);
    register_genesis(app, ctx);
    register_dev(app, ctx);
    register_query(app, ctx);

    try {
        app.parse(argc, argv);
        return kExitOk;
    } catch (const CLI::ParseError& e) {
        return app.exit(e) == 0 ? kExitOk : kExitUsage;
    } catch (const CliError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return e.code();
    } catch (const yap::dev::BlockRejected& e) {
        std::cerr << "error: block rejected: " << e.what() << "\n";
        return kExitRejected;
    } catch (const yap::chain::IncompatibleSchema& e) {
        std::cerr << "storage error: " << e.what() << "\n";
        return kExitStorage;
    } catch (const yap::storage::StorageError& e) {
        std::cerr << "storage error: " << e.what() << "\n";
        return kExitStorage;
    } catch (const yap::FailStop& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return kExitStorage;
    } catch (const std::exception& e) {
        std::cerr << "internal error: " << e.what() << "\n";
        return kExitInternal;
    }
}
