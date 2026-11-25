// SPDX-License-Identifier: Apache-2.0
//
// Bounded HTTP(S) POST over libcurl (ADR-0029). One request, no retries, no redirects, no proxies,
// http/https only, TLS peer and host verification always on (system CA store or an explicit CA file),
// TLS >= 1.2, bounded connect / stall / total time and response size. Nothing secret is ever passed
// here: requests carry public data only (queries, signed transactions).
#pragma once

#include <cstddef>
#include <stdexcept>
#include <string>

namespace yap::rpc {

struct HttpOptions {
    long connect_timeout_ms = 5'000;   ///< TCP + TLS handshake
    long read_timeout_ms = 15'000;     ///< abort if no byte arrives for this long (stall)
    long total_timeout_ms = 30'000;    ///< the whole request
    std::size_t max_response_bytes = 16u << 20;
    std::string ca_file;               ///< extra trust anchor (PEM); empty = system CA store
};

enum class TransportErrorKind {
    InvalidUrl,  ///< not http(s), credentials in the URL, unparsable
    Connect,     ///< DNS or TCP connect failed: the request was not sent
    Tls,         ///< TLS handshake / certificate verification failed: the request was not sent
    Timeout,     ///< connect, stall or total timeout
    TooLarge,    ///< response larger than max_response_bytes
    Disconnect,  ///< the connection broke while sending or receiving
    Other,
};

/// A transport failure. `request_sent` is true when request bytes may have reached the server: for a
/// broadcast that makes the outcome unknown (the transaction may or may not have been accepted).
class TransportError : public std::runtime_error {
public:
    TransportError(TransportErrorKind kind, bool request_sent, const std::string& msg)
        : std::runtime_error(msg), kind_(kind), request_sent_(request_sent) {}
    [[nodiscard]] TransportErrorKind kind() const noexcept { return kind_; }
    [[nodiscard]] bool request_sent() const noexcept { return request_sent_; }

private:
    TransportErrorKind kind_;
    bool request_sent_;
};

struct HttpResponse {
    long status = 0;
    std::string body;
};

/// Validates an endpoint URL: http:// or https://, a host, no user:password@, no fragment.
/// Returns an error message or an empty string.
[[nodiscard]] std::string check_endpoint(const std::string& url);

/// POSTs `body` (application/json) to `url`. Returns any HTTP status with its body (CometBFT answers
/// JSON-RPC errors with HTTP 500); throws TransportError on every transport failure.
[[nodiscard]] HttpResponse http_post(const std::string& url, const std::string& body, const HttpOptions& opts);

}  // namespace yap::rpc
