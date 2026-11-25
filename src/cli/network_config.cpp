// SPDX-License-Identifier: Apache-2.0
#include "network_config.hpp"

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>

#include "yap/chain/genesis.hpp"
#include "yap/tx/chain_id.hpp"

namespace yap::cli {
namespace {

using Json = nlohmann::ordered_json;

constexpr long kMaxTimeoutMs = 600'000;
constexpr std::uint64_t kMaxResponseBytes = 256ull << 20;

void check_chain_id(const std::string& id, const std::string& source) {
    if (!tx::is_valid_chain_id(id)) {
        throw CliError(kExitUsage, "invalid chain id '" + id + "' from " + source +
                                       " (allowed: [a-z0-9-], 1..32, no leading/trailing '-')");
    }
}

void check_timeout(long v, const char* name) {
    if (v < 100 || v > kMaxTimeoutMs) {
        throw CliError(kExitUsage, std::string(name) + " must be between 100 and " + std::to_string(kMaxTimeoutMs) + " ms");
    }
}

Json read_file_json(const std::filesystem::path& p) {
    std::ifstream in(p);
    if (!in) throw CliError(kExitUsage, "cannot read " + p.string());
    std::stringstream ss;
    ss << in.rdbuf();
    Json j = Json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw CliError(kExitUsage, p.string() + " is not a JSON object");
    return j;
}

}  // namespace

rpc::HttpOptions NetworkConfig::http() const {
    rpc::HttpOptions o;
    o.connect_timeout_ms = connect_timeout_ms.value;
    o.read_timeout_ms = read_timeout_ms.value;
    o.total_timeout_ms = timeout_ms.value;
    o.max_response_bytes = static_cast<std::size_t>(max_response_bytes.value);
    o.ca_file = ca_file.value;
    return o;
}

std::string NetworkConfig::hrp() const { return std::string(chain::hrp_for_chain(chain_id.value)); }

std::filesystem::path network_config_path(const Context& ctx) { return ctx.home / "network.json"; }

NetworkConfig load_network_config(const Context& ctx) {
    NetworkConfig c;
    const auto path = network_config_path(ctx);
    if (!std::filesystem::exists(path)) return c;
    const Json j = read_file_json(path);
    const std::string src = "config";
    for (const auto& [k, v] : j.items()) {
        const auto bad = [&] { return CliError(kExitUsage, path.string() + ": invalid value of '" + k + "'"); };
        if (k == "rpc") {
            if (!v.is_string()) throw bad();
            c.rpc = {v.get<std::string>(), src};
        } else if (k == "chain_id") {
            if (!v.is_string()) throw bad();
            c.chain_id = {v.get<std::string>(), src};
        } else if (k == "connect_timeout_ms" || k == "read_timeout_ms" || k == "timeout_ms") {
            if (!v.is_number_integer()) throw bad();
            auto& s = k == "connect_timeout_ms" ? c.connect_timeout_ms : k == "read_timeout_ms" ? c.read_timeout_ms : c.timeout_ms;
            s = {v.get<long>(), src};
        } else if (k == "max_response_bytes") {
            if (!v.is_number_unsigned()) throw bad();
            c.max_response_bytes = {v.get<std::uint64_t>(), src};
        } else if (k == "ca_file") {
            if (!v.is_string()) throw bad();
            c.ca_file = {v.get<std::string>(), src};
        } else {
            throw CliError(kExitUsage, path.string() + ": unknown key '" + k + "'");
        }
    }
    return c;
}

NetworkConfig resolve_network_config(const Context& ctx, const NetworkFlags& f) {
    NetworkConfig c = load_network_config(ctx);
    if (const char* e = std::getenv("YAP_RPC"); e != nullptr && *e != '\0') c.rpc = {e, "env"};
    if (const char* e = std::getenv("YAP_CHAIN_ID"); e != nullptr && *e != '\0') c.chain_id = {e, "env"};
    if (!f.rpc.empty()) c.rpc = {f.rpc, "flag"};
    if (!f.chain_id.empty()) c.chain_id = {f.chain_id, "flag"};
    if (f.connect_timeout_ms) c.connect_timeout_ms = {*f.connect_timeout_ms, "flag"};
    if (f.read_timeout_ms) c.read_timeout_ms = {*f.read_timeout_ms, "flag"};
    if (f.timeout_ms) c.timeout_ms = {*f.timeout_ms, "flag"};
    if (f.max_response_bytes) c.max_response_bytes = {*f.max_response_bytes, "flag"};
    if (!f.ca_file.empty()) c.ca_file = {f.ca_file, "flag"};

    if (const auto e = rpc::check_endpoint(c.rpc.value); !e.empty()) {
        throw CliError(kExitUsage, e + " (rpc from " + c.rpc.source + ": '" + c.rpc.value + "')");
    }
    if (c.has_chain_id()) check_chain_id(c.chain_id.value, c.chain_id.source);
    check_timeout(c.connect_timeout_ms.value, "connect timeout");
    check_timeout(c.read_timeout_ms.value, "read timeout");
    check_timeout(c.timeout_ms.value, "timeout");
    if (c.max_response_bytes.value < 4096 || c.max_response_bytes.value > kMaxResponseBytes) {
        throw CliError(kExitUsage, "max response bytes must be between 4096 and " + std::to_string(kMaxResponseBytes));
    }
    if (!c.ca_file.value.empty() && !std::filesystem::is_regular_file(c.ca_file.value)) {
        throw CliError(kExitUsage, "CA file " + c.ca_file.value + " does not exist");
    }
    return c;
}

void save_network_config(const Context& ctx, const NetworkFlags& v) {
    const auto path = network_config_path(ctx);
    Json j = std::filesystem::exists(path) ? read_file_json(path) : Json::object();
    if (!v.rpc.empty()) {
        if (const auto e = rpc::check_endpoint(v.rpc); !e.empty()) throw CliError(kExitUsage, e);
        j["rpc"] = v.rpc;
    }
    if (!v.chain_id.empty()) {
        check_chain_id(v.chain_id, "--chain-id");
        j["chain_id"] = v.chain_id;
    }
    if (v.connect_timeout_ms) j["connect_timeout_ms"] = *v.connect_timeout_ms;
    if (v.read_timeout_ms) j["read_timeout_ms"] = *v.read_timeout_ms;
    if (v.timeout_ms) j["timeout_ms"] = *v.timeout_ms;
    if (v.max_response_bytes) j["max_response_bytes"] = *v.max_response_bytes;
    if (!v.ca_file.empty()) j["ca_file"] = std::filesystem::absolute(v.ca_file).string();
    std::filesystem::create_directories(ctx.home);
    const std::string text = j.dump(2) + "\n";
    write_file_bytes(path, as_bytes(text), /*overwrite=*/true);
    // Validate the merged result exactly as it will be read.
    (void)resolve_network_config(ctx, NetworkFlags{});
}

}  // namespace yap::cli
