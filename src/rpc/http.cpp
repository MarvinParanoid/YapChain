// SPDX-License-Identifier: Apache-2.0
#include "yap/rpc/http.hpp"

#include <curl/curl.h>

#include <chrono>
#include <memory>
#include <mutex>

namespace yap::rpc {
namespace {

void global_init() {
    static std::once_flag once;
    std::call_once(once, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw TransportError(TransportErrorKind::Other, false, "libcurl initialisation failed");
        }
    });
}

struct Sink {
    std::string body;
    std::size_t limit = 0;
    bool too_large = false;
    // Stall detection: time of the last progress (request start, any byte sent or received).
    std::chrono::steady_clock::time_point last_progress = std::chrono::steady_clock::now();
    curl_off_t last_dl = 0, last_ul = 0;
    long stall_ms = 0;
    bool stalled = false;
};

extern "C" int progress_cb(void* user, curl_off_t, curl_off_t dlnow, curl_off_t, curl_off_t ulnow) {
    auto* s = static_cast<Sink*>(user);
    const auto now = std::chrono::steady_clock::now();
    if (dlnow != s->last_dl || ulnow != s->last_ul) {
        s->last_dl = dlnow;
        s->last_ul = ulnow;
        s->last_progress = now;
        return 0;
    }
    if (now - s->last_progress > std::chrono::milliseconds(s->stall_ms)) {
        s->stalled = true;
        return 1;  // aborts the transfer (CURLE_ABORTED_BY_CALLBACK)
    }
    return 0;
}

extern "C" std::size_t write_cb(char* data, std::size_t size, std::size_t n, void* user) {
    auto* s = static_cast<Sink*>(user);
    const std::size_t len = size * n;
    if (len > s->limit - s->body.size()) {  // body.size() <= limit always holds
        s->too_large = true;
        return 0;  // aborts the transfer (CURLE_WRITE_ERROR)
    }
    s->body.append(data, len);
    return len;
}

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

}  // namespace

std::string check_endpoint(const std::string& url) {
    std::string rest;
    if (starts_with(url, "http://")) {
        rest = url.substr(7);
    } else if (starts_with(url, "https://")) {
        rest = url.substr(8);
    } else {
        return "the RPC endpoint must start with http:// or https://";
    }
    const auto host_end = rest.find_first_of("/?#");
    const std::string authority = rest.substr(0, host_end);
    if (authority.empty()) return "the RPC endpoint has no host";
    if (authority.contains('@')) {
        return "the RPC endpoint must not contain credentials (user:password@)";
    }
    if (url.contains('#')) return "the RPC endpoint must not contain a fragment";
    for (const char c : url) {
        if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7f) {
            return "the RPC endpoint contains spaces or non-ASCII characters";
        }
    }
    return {};
}

HttpResponse http_post(const std::string& url, const std::string& body, const HttpOptions& opts) {
    if (const auto err = check_endpoint(url); !err.empty()) {
        throw TransportError(TransportErrorKind::InvalidUrl, false, err);
    }
    global_init();
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> h(curl_easy_init(), curl_easy_cleanup);
    if (!h) throw TransportError(TransportErrorKind::Other, false, "libcurl: cannot create a handle");
    CURL* c = h.get();

    Sink sink;
    sink.limit = opts.max_response_bytes;
    char errbuf[CURL_ERROR_SIZE] = {0};
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(
        curl_slist_append(nullptr, "Content-Type: application/json"), curl_slist_free_all);
    if (!headers) throw TransportError(TransportErrorKind::Other, false, "libcurl: out of memory");

    sink.stall_ms = opts.read_timeout_ms <= 0 ? 1 : opts.read_timeout_ms;
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(c, CURLOPT_PROXY, "");  // never through an environment-configured proxy
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(c, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    if (!opts.ca_file.empty()) curl_easy_setopt(c, CURLOPT_CAINFO, opts.ca_file.c_str());
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, opts.connect_timeout_ms);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, opts.total_timeout_ms);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);  // the progress callback implements the stall timeout
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &sink);
    curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(opts.max_response_bytes));
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_ERRORBUFFER, errbuf);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "yap-cli");

    const CURLcode rc = curl_easy_perform(c);
    long sent = 0;
    curl_easy_getinfo(c, CURLINFO_REQUEST_SIZE, &sent);
    const bool request_sent = sent > 0;
    const std::string detail = errbuf[0] != '\0' ? std::string(errbuf) : std::string(curl_easy_strerror(rc));
    switch (rc) {
        case CURLE_OK:
            break;
        case CURLE_UNSUPPORTED_PROTOCOL:
        case CURLE_URL_MALFORMAT:
            throw TransportError(TransportErrorKind::InvalidUrl, false, "invalid RPC endpoint: " + detail);
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
        case CURLE_COULDNT_CONNECT:
            throw TransportError(TransportErrorKind::Connect, false, "cannot connect to the RPC endpoint: " + detail);
        case CURLE_PEER_FAILED_VERIFICATION:
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_SSL_CERTPROBLEM:
        case CURLE_SSL_CIPHER:
        case CURLE_SSL_CACERT_BADFILE:
        case CURLE_SSL_ISSUER_ERROR:
            throw TransportError(TransportErrorKind::Tls, false, "TLS verification failed: " + detail);
        case CURLE_ABORTED_BY_CALLBACK:
            throw TransportError(TransportErrorKind::Timeout, request_sent,
                                 "RPC endpoint stalled: no data for " + std::to_string(opts.read_timeout_ms) + " ms");
        case CURLE_OPERATION_TIMEDOUT:
            throw TransportError(TransportErrorKind::Timeout, request_sent, "RPC request timed out: " + detail);
        case CURLE_FILESIZE_EXCEEDED:
            throw TransportError(TransportErrorKind::TooLarge, true,
                                 "RPC response larger than " + std::to_string(opts.max_response_bytes) + " bytes");
        case CURLE_WRITE_ERROR:
            if (sink.too_large) {
                throw TransportError(TransportErrorKind::TooLarge, true, "RPC response larger than " +
                                                                             std::to_string(opts.max_response_bytes) +
                                                                             " bytes");
            }
            throw TransportError(TransportErrorKind::Other, true, "RPC response: " + detail);
        case CURLE_GOT_NOTHING:
        case CURLE_RECV_ERROR:
        case CURLE_SEND_ERROR:
        case CURLE_PARTIAL_FILE:
            throw TransportError(TransportErrorKind::Disconnect, true, "RPC connection broke: " + detail);
        default:
            throw TransportError(TransportErrorKind::Other, request_sent, "RPC transport error: " + detail);
    }
    HttpResponse r;
    curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
    r.body = std::move(sink.body);
    return r;
}

}  // namespace yap::rpc
