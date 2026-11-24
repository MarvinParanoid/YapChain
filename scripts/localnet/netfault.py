#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Controllable P2P fault injection for the YapChain localnet (M2.5). Test tooling only.

An unprivileged TCP proxy (asyncio, standard library) sits on every directed P2P link of a localnet
created with `localnet.py init --validators N --fault-proxy`: node i reaches node j only through its
own proxy port for (i, j) — node i's persistent_peers name j at that port, PEX is off, so there is no
other route. RPC (TCP) and ABCI (Unix socket) never pass through the proxy.

Faults, switchable at run time through a control socket (<dir>/fault.sock, one JSON object per line):
  {"cmd": "status"}                                        links, open connections, bytes, rules
  {"cmd": "partition", "groups": [[0, 1, 2], [3]]}         only links inside a group pass; nodes in no
                                                           group are isolated from everyone
  {"cmd": "heal"}                                          remove the partition
  {"cmd": "delay", "ms": 150, "jitter_ms": 50}             ONE-WAY delay per direction (RTT = 2 x ms),
                                                           uniform jitter +-jitter_ms, order preserved
  {"cmd": "flap", "period_s": 5, "down_s": 1}              periodic outage: every period_s all P2P links go
                                                           down (connections closed) for down_s; 0 stops
  {"cmd": "reset", "probability": 0.2}                     close each open connection with this probability
                                                           (TCP connection resets — NOT packet loss)
  {"cmd": "reload"}                                        re-read network.json (links of added nodes)
Individual packet loss is not modelled: it needs tc/netem (root), which this tool must not require.

Every fault event is appended to <dir>/faults.log with a UTC timestamp.

    netfault.py serve  --dir D        (started by localnet.py / the tests; exits with its parent)
    netfault.py ctl    --dir D JSON   (one control command, prints the JSON answer)
"""
import argparse
import asyncio
import datetime
import json
import os
import random
import socket
import sys
import time


def now_iso():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z"


class Link:
    def __init__(self, src, dst, port, target):
        self.src, self.dst, self.port, self.target = src, dst, port, target
        self.conns = set()          # open (client_writer, upstream_writer) pairs
        self.bytes = [0, 0]         # src->dst, dst->src (total)
        self.accepted = 0
        self.refused = 0
        self.delayed = 0            # chunks held back by the delay rule
        self.delay_sum_ms = 0.0     # total delay actually applied
        self.server = None

    def key(self):
        return f"{self.src}->{self.dst}"


class Proxy:
    def __init__(self, d, seed=None):
        self.d = d
        self.links = {}
        self.groups = None          # None: no partition
        self.delay_ms = 0.0
        self.jitter_ms = 0.0
        self.flap = None            # (period_s, down_s)
        self.flap_down = False
        self.rng = random.Random(seed)
        self.log_path = os.path.join(d, "faults.log")

    # ---- rules
    def allowed(self, link):
        if self.flap_down:
            return False
        if self.groups is None:
            return True
        return any(link.src in g and link.dst in g for g in self.groups)

    def event(self, what, **kw):
        rec = {"time": now_iso(), "event": what, **kw}
        with open(self.log_path, "a") as f:
            f.write(json.dumps(rec) + "\n")
        return rec

    def close_blocked(self):
        n = 0
        for link in self.links.values():
            if not self.allowed(link):
                for cw, uw in list(link.conns):
                    for w in (cw, uw):
                        w.transport.abort()
                    n += 1
        return n

    # ---- data path
    async def pump(self, link, reader, writer, direction):
        """Copies one direction; each chunk is delivered `delay +- jitter` after it was read, never
        before an earlier chunk (TCP order)."""
        last = 0.0
        loop = asyncio.get_running_loop()
        try:
            while True:
                data = await reader.read(65536)
                if not data:
                    break
                if not self.allowed(link):
                    break
                if self.delay_ms or self.jitter_ms:
                    j = self.rng.uniform(-self.jitter_ms, self.jitter_ms) if self.jitter_ms else 0.0
                    due = max(loop.time() + max(0.0, self.delay_ms + j) / 1000.0, last)
                    last = due
                    wait = due - loop.time()
                    if wait > 0:
                        link.delayed += 1
                        link.delay_sum_ms += wait * 1000.0
                        await asyncio.sleep(wait)
                    if not self.allowed(link):
                        break
                writer.write(data)
                link.bytes[direction] += len(data)
                await writer.drain()
        except (ConnectionError, OSError, asyncio.CancelledError):
            pass
        finally:
            writer.transport.abort()

    async def handle(self, link, creader, cwriter):
        if not self.allowed(link):
            link.refused += 1
            cwriter.transport.abort()
            return
        try:
            ureader, uwriter = await asyncio.open_connection("127.0.0.1", link.target)
        except OSError:
            cwriter.transport.abort()
            return
        for w in (cwriter, uwriter):
            sock = w.get_extra_info("socket")
            if sock is not None:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        link.accepted += 1
        pair = (cwriter, uwriter)
        link.conns.add(pair)
        try:
            await asyncio.gather(self.pump(link, creader, uwriter, 0), self.pump(link, ureader, cwriter, 1))
        finally:
            link.conns.discard(pair)
            cwriter.transport.abort()
            uwriter.transport.abort()

    async def add_link(self, src, dst, port, target):
        key = f"{src}->{dst}"
        if key in self.links:
            return
        link = Link(src, dst, port, target)
        link.server = await asyncio.start_server(lambda r, w: self.handle(link, r, w), "127.0.0.1", port)
        self.links[key] = link

    async def load(self):
        with open(os.path.join(self.d, "network.json")) as f:
            net = json.load(f)
        for x in net.get("links", []):
            await self.add_link(x["src"], x["dst"], x["port"], x["target"])

    async def flapper(self):
        while True:
            if not self.flap:
                await asyncio.sleep(0.2)
                continue
            period, down = self.flap
            await asyncio.sleep(max(0.0, period - down))
            if not self.flap:
                continue
            self.flap_down = True
            closed = self.close_blocked()
            self.event("flap_down", closed=closed, down_s=down)
            await asyncio.sleep(down)
            self.flap_down = False
            self.event("flap_up")

    # ---- control
    def status(self):
        return {"groups": self.groups, "delay_ms": self.delay_ms, "jitter_ms": self.jitter_ms,
                "flap": self.flap, "flap_down": self.flap_down,
                "links": {k: {"src": l.src, "dst": l.dst, "port": l.port, "target": l.target,
                              "open": len(l.conns), "bytes": list(l.bytes), "accepted": l.accepted,
                              "refused": l.refused, "allowed": self.allowed(l), "delayed": l.delayed,
                              "mean_delay_ms": (l.delay_sum_ms / l.delayed) if l.delayed else 0.0} for k, l in self.links.items()}}

    async def command(self, req):
        cmd = req.get("cmd")
        if cmd == "status":
            return {"ok": True, **self.status()}
        if cmd == "partition":
            groups = [sorted(set(int(x) for x in g)) for g in req["groups"]]
            self.groups = groups
            closed = self.close_blocked()
            return {"ok": True, **self.event("partition", groups=groups, closed=closed)}
        if cmd == "heal":
            self.groups = None
            return {"ok": True, **self.event("heal")}
        if cmd == "delay":
            self.delay_ms = float(req.get("ms", 0))
            self.jitter_ms = float(req.get("jitter_ms", 0))
            return {"ok": True, **self.event("delay", one_way_ms=self.delay_ms, jitter_ms=self.jitter_ms)}
        if cmd == "flap":
            period, down = float(req.get("period_s", 0)), float(req.get("down_s", 0))
            self.flap = (period, down) if period > 0 and 0 < down < period else None
            return {"ok": True, **self.event("flap", period_s=period, down_s=down)}
        if cmd == "reset":
            p = float(req.get("probability", 1.0))
            n = 0
            for link in self.links.values():
                for cw, uw in list(link.conns):
                    if self.rng.random() < p:
                        cw.transport.abort()
                        uw.transport.abort()
                        n += 1
            return {"ok": True, **self.event("reset", probability=p, closed=n)}
        if cmd == "reload":
            await self.load()
            return {"ok": True, **self.event("reload", links=len(self.links))}
        return {"ok": False, "error": f"unknown command {cmd!r}"}

    async def control(self, reader, writer):
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                try:
                    resp = await self.command(json.loads(line))
                except (ValueError, KeyError, TypeError) as e:
                    resp = {"ok": False, "error": str(e)}
                writer.write((json.dumps(resp) + "\n").encode())
                await writer.drain()
        finally:
            writer.close()

    async def serve(self):
        await self.load()
        path = os.path.join(self.d, "fault.sock")
        if os.path.exists(path):
            os.remove(path)
        server = await asyncio.start_unix_server(self.control, path)
        os.chmod(path, 0o600)
        self.event("start", links=len(self.links), pid=os.getpid())
        asyncio.get_running_loop().create_task(self.flapper())
        async with server:
            await server.serve_forever()


def ctl(d, req, timeout=10):
    """One control command (used by localnet.py and the tests)."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as s:
        s.settimeout(timeout)
        s.connect(os.path.join(d, "fault.sock"))
        s.sendall((json.dumps(req) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    resp = json.loads(buf)
    if not resp.get("ok"):
        raise RuntimeError(f"netfault: {resp.get('error')}")
    return resp


def wait_ready(d, timeout=20):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            return ctl(d, {"cmd": "status"}, timeout=2)
        except (OSError, ValueError, RuntimeError):
            time.sleep(0.1)
    raise RuntimeError("netfault proxy did not start")


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("serve")
    s.add_argument("--dir", required=True)
    s.add_argument("--seed", type=int)
    c = sub.add_parser("ctl")
    c.add_argument("--dir", required=True)
    c.add_argument("json")
    a = ap.parse_args(argv)
    if a.cmd == "serve":
        try:
            asyncio.run(Proxy(os.path.abspath(a.dir), a.seed).serve())
        except KeyboardInterrupt:
            pass
        return 0
    print(json.dumps(ctl(os.path.abspath(a.dir), json.loads(a.json)), indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
