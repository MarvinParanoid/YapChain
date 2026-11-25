# SPDX-License-Identifier: Apache-2.0
"""A scriptable mock of the CometBFT JSON-RPC endpoint for CLI tests (standard library only).

    with MockRpc(handler) as m:  m.url, m.requests
handler(method, params) returns one of:
    ("result", obj) | ("error", code, message, data) | ("raw", status, body_bytes)
    | ("close",)  (close without answering) | ("stall", seconds) (accept, never answer in time)
    | ("huge", n_bytes)  (stream n bytes of garbage)
"""
import base64
import json
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class MockRpc:
    def __init__(self, handler, tls_context=None):
        self.handler = handler
        self.requests = []
        outer = self

        class H(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *a):  # quiet
                pass

            def do_POST(self):
                n = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(n)
                req = json.loads(body)
                outer.requests.append({"method": req["method"], "params": req.get("params"),
                                       "headers": dict(self.headers), "raw": body.decode()})
                r = outer.handler(req["method"], req.get("params") or {})
                kind = r[0]
                if kind == "close":
                    self.close_connection = True
                    self.connection.shutdown(socket.SHUT_RDWR)
                    return
                if kind == "stall":
                    time.sleep(r[1])
                    self.close_connection = True
                    return
                if kind == "huge":
                    self.send_response(200)
                    self.send_header("Content-Type", "application/json")
                    self.send_header("Content-Length", str(r[1]))
                    self.end_headers()
                    chunk = b"x" * 65536
                    left = r[1]
                    try:
                        while left > 0:
                            self.wfile.write(chunk[:min(left, len(chunk))])
                            left -= len(chunk)
                    except (BrokenPipeError, ConnectionResetError):
                        pass
                    return
                if kind == "raw":
                    status, data = r[1], r[2]
                elif kind == "result":
                    status, data = 200, json.dumps({"jsonrpc": "2.0", "id": req["id"], "result": r[1]}).encode()
                else:
                    err = {"code": r[1], "message": r[2], "data": r[3]}
                    status, data = 500, json.dumps({"jsonrpc": "2.0", "id": req["id"], "error": err}).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.server = ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.server.daemon_threads = True
        self.server.handle_error = lambda *a: None  # clients that abort (TLS refusal, size limit) are expected
        if tls_context is not None:
            self.server.socket = tls_context.wrap_socket(self.server.socket, server_side=True)
        scheme = "https" if tls_context is not None else "http"
        host = "localhost" if tls_context is not None else "127.0.0.1"
        self.url = f"{scheme}://{host}:{self.server.server_address[1]}"
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *a):
        self.server.shutdown()
        self.server.server_close()

    def count(self, method):
        return sum(1 for r in self.requests if r["method"] == method)


def status_result(chain_id="yapchain-localnet-1", height=10, catching_up=False):
    return {"node_info": {"id": "ab" * 20, "network": chain_id, "moniker": "mock", "version": "0.39.0"},
            "sync_info": {"latest_block_hash": "AA" * 32, "latest_app_hash": "BB" * 32,
                          "latest_block_height": str(height), "latest_block_time": "2026-11-25T00:00:00Z",
                          "earliest_block_height": "1", "catching_up": catching_up},
            "validator_info": {"voting_power": "0"}}


def query_result(obj, height=10, code=0):
    return {"response": {"code": code, "log": "", "height": str(height),
                         "value": base64.b64encode(json.dumps(obj).encode()).decode() if obj is not None else None}}


def chain_answer(chain_id="yapchain-localnet-1", height=10):
    return {"chain_id": chain_id, "height": str(height), "state_root": "00" * 32, "genesis_hash": "11" * 32,
            "last_time_ms": "1", "total_supply": "5", "total_minted": "5", "total_burned": "0"}
