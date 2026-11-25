// SPDX-License-Identifier: Apache-2.0
//
// Network mode of the `yap` CLI (M2.6): which CometBFT RPC endpoint to talk to and which chain the
// user expects. Local dev mode (yap dev / yap query / yap tx) never reads this.
//
// Precedence, per setting: command-line flag  >  environment (YAP_RPC, YAP_CHAIN_ID)  >
// <home>/network.json  >  built-in default. The expected chain id has NO default and is never taken
// from the RPC node: it is what the user signs for.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "context.hpp"
#include "yap/rpc/http.hpp"

namespace yap::cli {

/// Network-mode exit codes (in addition to the local ones in context.hpp, which keep their meaning).
enum NetExitCode : int {
    kExitNetwork = 6,    ///< RPC unreachable, TLS failure, HTTP error, malformed answer: nothing happened
    kExitNotFound = 7,   ///< the transaction / block is not known to the node
    kExitUnknown = 8,    ///< outcome unknown: broadcast interrupted, or not final before the timeout
    kExitTxFailed = 9,   ///< finalized, but execution returned an error code
    kExitChainMismatch = 10,  ///< the node or the transaction is for another chain than expected
};

inline constexpr const char* kDefaultRpc = "http://127.0.0.1:26657";

/// A failure whose result was already printed (with --json, as the command's JSON object): main
/// only reports the message on stderr and exits with the code.
class ReportedError : public CliError {
public:
    using CliError::CliError;
};

/// One setting with where it came from ("flag", "env", "config", "default").
template <class T>
struct Setting {
    T value{};
    std::string source = "default";
};

struct NetworkConfig {
    Setting<std::string> rpc{kDefaultRpc, "default"};
    Setting<std::string> chain_id{"", "unset"};
    Setting<long> connect_timeout_ms{5'000, "default"};
    Setting<long> read_timeout_ms{15'000, "default"};
    Setting<long> timeout_ms{30'000, "default"};
    Setting<std::uint64_t> max_response_bytes{16u << 20, "default"};
    Setting<std::string> ca_file{"", "default"};

    [[nodiscard]] rpc::HttpOptions http() const;
    [[nodiscard]] bool has_chain_id() const { return !chain_id.value.empty(); }
    /// The address prefix of the expected chain (protocol §11.3); "tyap" until a mainnet exists.
    [[nodiscard]] std::string hrp() const;
};

/// Flags given on the command line (empty / unset = not given).
struct NetworkFlags {
    std::string rpc;
    std::string chain_id;
    std::optional<long> connect_timeout_ms, read_timeout_ms, timeout_ms;
    std::optional<std::uint64_t> max_response_bytes;
    std::string ca_file;
    bool json = false;
};

[[nodiscard]] std::filesystem::path network_config_path(const Context& ctx);

/// Reads <home>/network.json (strict: known keys only, typed values) if it exists.
[[nodiscard]] NetworkConfig load_network_config(const Context& ctx);

/// Applies environment and flags over the file / defaults and validates everything.
[[nodiscard]] NetworkConfig resolve_network_config(const Context& ctx, const NetworkFlags& flags);

/// Writes the file settings (only the keys present in `file_values`, merged with the existing file).
void save_network_config(const Context& ctx, const NetworkFlags& file_values);

}  // namespace yap::cli
