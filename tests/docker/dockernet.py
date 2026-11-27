# SPDX-License-Identifier: Apache-2.0
"""Library of the container tests (M2.7): a YapChain network made of the real images, created with
scripts/ops/yapnode.py and run with the deploy/compose files, adapted to the independent state model
and the strict per-height checks of tests/integration/livenet.py.

Nodes are compared strictly by height (block hash, header app hash, execution results, each yapd's own
committed state root from its log). yapd logs are collected from `docker logs` and archived before a
container is removed, so "every height committed exactly once per node" is checked across restarts,
crashes and `compose down/up`.
"""
import base64
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tests", "integration"))
sys.path.insert(0, os.path.join(ROOT, "scripts", "ops"))
import livenet  # noqa: E402
from livenet import Fail, check, wait_until  # noqa: E402,F401
import yapnode  # noqa: E402
import localnet as ln  # noqa: E402

DOCKER = os.environ.get("YAP_DOCKER", "docker")
COMPOSE_DIR = os.path.join(ROOT, "deploy", "compose")


def sh(cmd, check_rc=True, timeout=600, input_text=None):
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, input=input_text)
    if check_rc and r.returncode != 0:
        raise Fail(f"{' '.join(cmd)} -> {r.returncode}\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
    return r


def step(n, text):
    livenet.step(n, text)


class CNode:
    """One containerised node: compose services <name>-yapd and <name> (CometBFT)."""

    def __init__(self, net, name, info, index):
        self.net, self.name, self.i = net, name, index
        self.dir = info["dir"]
        self.validator = info["role"] == "validator"
        self.address = info["validator_address"].upper()
        self.reward = net.addresses.get(f"reward-{name}")
        self.meta = {"rpc": info["rpc"]}
        self.archived = ""          # yapd log text of containers that no longer exist

    def __repr__(self):
        return self.name

    # containers
    def svc(self, which):
        return f"{self.name}-yapd" if which == "yapd" else self.name

    def container(self, which):
        return f"{self.net.project}-{self.svc(which)}-1"

    def inspect(self, which):
        r = sh([DOCKER, "inspect", self.container(which)], check_rc=False)
        return json.loads(r.stdout)[0] if r.returncode == 0 else None

    def state(self, which):
        c = self.inspect(which)
        if c is None:
            return {"exists": False, "running": False, "health": None, "restarts": 0, "pid": 0}
        st = c["State"]
        return {"exists": True, "running": st["Running"], "health": (st.get("Health") or {}).get("Status"),
                "restarts": c.get("RestartCount", 0), "pid": st.get("Pid", 0), "exit": st.get("ExitCode"),
                "status": st["Status"], "id": c["Id"]}

    def alive(self, which="cometbft"):
        return self.state(which)["running"]

    def crash(self, which):
        """An unexpected termination: SIGKILL to the container's main process from the host (the
        container runs as our uid). Docker sees a crash, not a user stop, so the restart policy applies."""
        pid = self.state(which)["pid"]
        check(pid > 0, f"{self}/{which}: not running")
        os.kill(pid, signal.SIGKILL)
        self.net.event(f"SIGKILL {self}/{which} (host pid {pid})")
        return pid

    def compose(self, *args, **kw):
        return self.net.compose(*args, **kw)

    def stop(self, which=None):
        svcs = [self.svc("cometbft"), self.svc("yapd")] if which is None else [self.svc(which)]
        self.compose("stop", *svcs)
        self.net.event(f"compose stop {' '.join(svcs)}")

    def start(self, which=None):
        svcs = [self.svc("yapd"), self.svc("cometbft")] if which is None else [self.svc(which)]
        self.compose("start", *svcs)
        self.net.event(f"compose start {' '.join(svcs)}")

    # RPC (same helpers as the process-based tests)
    def rpc(self, method, **params):
        return ln.rpc(self.meta, method, **params)

    def height(self):
        return ln.height(self.meta)

    def block(self, h):
        return self.rpc("block", height=h)

    def header(self, h):
        return self.block(h)["block"]["header"]

    def peers(self):
        return int(self.rpc("net_info")["n_peers"])

    def query(self, path):
        r = self.rpc("abci_query", path=f'"{path}"')["response"]
        check(int(r.get("code", 0)) == 0, f"{self}: query {path}: {r}")
        return int(r["height"]), json.loads(base64.b64decode(r["value"]))

    def broadcast(self, tx_hex):
        return self.rpc("broadcast_tx_sync", tx="0x" + tx_hex)

    def catching_up(self):
        return bool(self.rpc("status")["sync_info"]["catching_up"])

    # logs
    def current_log(self, which="yapd"):
        r = sh([DOCKER, "logs", self.container(which)], check_rc=False)
        return (r.stdout + r.stderr) if r.returncode == 0 else ""

    def log(self, name="yapd", since=0):
        text = (self.archived if name == "yapd" else "") + self.current_log(name)
        return text[since:]

    def archive_logs(self):
        self.archived += self.current_log("yapd")

    def commits(self):
        return [(int(h), r) for h, r in re.findall(r"Commit: height (\d+), app_hash ([0-9a-f]{64})", self.log())]

    def roots(self):
        out = {}
        for h, r in self.commits():
            check(h not in out, f"{self}: height {h} committed twice")
            out[h] = r
        return out


class DockerNet:
    """N validators (+ full nodes) from yapnode.localnet_init, run with deploy/compose."""

    def __init__(self, base, validators=4, full_nodes=0, project="yaptest", timeout_commit="1s",
                 extra_accounts=None, rpc_port_base=36657, subnet_prefix="172.31.250"):
        self.base = os.path.abspath(base)
        r = sh([DOCKER, "compose", "ls", "-a", "-q"], check_rc=False)
        check(project not in r.stdout.split(), f"compose project {project} already exists: remove it first "
                                               f"(docker compose -p {project} down)")
        self.d = os.path.join(self.base, "net")
        self.project = project
        self.events = []
        self.files = ["devnet.yml"] if validators == 1 else ["localnet.yml"] + (["fullnode.yml"] if full_nodes else [])
        self.info = yapnode.localnet_init(self.d, validators, full_nodes, f"yapchain-{project}-1", timeout_commit,
                                          rpc_port_base=rpc_port_base, extra_accounts=extra_accounts, project=project,
                                          subnet_prefix=subnet_prefix)
        self.addresses = self.info["addresses"]
        self.extra_accounts = dict(extra_accounts or {})
        self.nodes = [CNode(self, name, x, i) for i, (name, x) in enumerate(self.info["nodes"].items())]
        self.validators = [n for n in self.nodes if n.validator]
        self.by_name = {n.name: n for n in self.nodes}
        self.info["wallet_home"] = self.info["wallet"]

    def event(self, text):
        rec = f"{time.strftime('%H:%M:%S')}.{int(time.time() * 1000) % 1000:03d} {text}"
        self.events.append(rec)
        with open(os.path.join(self.base, "events.log"), "a") as f:
            f.write(rec + "\n")
        print(f"    [{rec}]", flush=True)
        return rec

    def compose(self, *args, files=None, check_rc=True, timeout=600):
        cmd = [DOCKER, "compose", "--project-directory", COMPOSE_DIR, "--env-file", os.path.join(self.d, "compose.env"),
               "-p", self.project]
        for f in files or self.files:
            cmd += ["-f", os.path.join(COMPOSE_DIR, f)]
        return sh(cmd + list(args), check_rc=check_rc, timeout=timeout)

    def up(self, services=(), wait=True, timeout=240):
        args = ["up", "-d"] + (["--wait", "--wait-timeout", str(timeout)] if wait else []) + list(services)
        t0 = time.monotonic()
        self.compose(*args, timeout=timeout + 60)
        self.event(f"compose up {' '.join(services) or '(all)'} ({time.monotonic() - t0:.1f}s)")
        return time.monotonic() - t0

    def down(self):
        for n in self.nodes:
            n.archive_logs()
        self.compose("down", "--timeout", "60")
        self.event("compose down")

    def running(self):
        return [n for n in self.nodes if n.alive("cometbft") and n.alive("yapd")]

    def min_height(self, nodes=None):
        return min(n.height() for n in (nodes or self.running()))

    def wait_height(self, h, nodes=None, timeout=180):
        nodes = nodes or self.running()
        wait_until(lambda: all(n.height() >= h for n in nodes), timeout, f"height {h} on {nodes}")
        return self.min_height(nodes)

    # transactions: built and signed by the real CLI inside the yapchain image (remote signing), sent
    # once through the node's RPC by the CLI itself
    def yap(self, *args, network="none", check_rc=True, extra_mounts=()):
        mounts = [(self.info["wallet"], "/wallet", "rw"), (self.info["password_file"], "/wallet.password", "ro")]
        mounts += list(extra_mounts)
        r = yapnode.yap(["--home", "/wallet", "--password-file", "/wallet.password", *args], mounts,
                        self.info["uid"], self.info["gid"], network=network, check=False)
        if check_rc and r.returncode != 0:
            raise Fail(f"yap {' '.join(args)} -> {r.returncode}\n{r.stdout}{r.stderr}")
        return r

    def make_tx(self, kind, sender, nonce, amount, to=None, expires=None, fee=livenet.FEE, node=None):
        self.tx_counter = getattr(self, "tx_counter", 0) + 1
        out_dir = os.path.join(self.base, "txs")
        os.makedirs(out_dir, exist_ok=True)
        os.chmod(out_dir, 0o777)
        name = f"tx-{self.tx_counter}.hex"
        expires = expires if expires is not None else (node or self.running()[0]).height() + 100
        args = ["tx", kind, "--from", sender, "--amount", str(amount), "--fee", str(fee), "--nonce", str(nonce),
                "--expires", str(expires), "--chain-id", self.info["chain_id"], "--out", f"/txs/{name}", "--yes"]
        if kind == "transfer":
            args += ["--to", self.addresses[to]]
        self.yap(*args, extra_mounts=[(out_dir, "/txs", "rw")])
        tx_hex = open(os.path.join(out_dir, name)).read().strip()
        import hashlib
        return tx_hex, hashlib.sha256(bytes.fromhex(tx_hex)).hexdigest().upper()

    def find_tx(self, h, nodes=None):
        out = {}
        for n in nodes or self.running():
            try:
                r = n.rpc("tx", hash="0x" + h)
                out[n.i] = (int(r["height"]), int(r["tx_result"].get("code", 0)))
            except (ln.LocalnetError, OSError):
                pass
        return out

    def wait_included(self, h, timeout=90, nodes=None):
        nodes = nodes or self.running()
        found = wait_until(lambda: (lambda f: f if len(f) == len(nodes) else None)(self.find_tx(h, nodes)),
                           timeout, f"transaction {h[:16]}… on every node")
        check(len(set(found.values())) == 1, f"transaction {h[:16]}…: nodes disagree {found}")
        height, code = next(iter(found.values()))
        check(code == 0, f"transaction {h[:16]}… included with code {code}")
        return height

    def verify_state(self, node):
        """`yap verify-state` on the node's database (node must be stopped: read-only open, no lock)."""
        r = yapnode.yap(["--home", "/var/lib/yapd", "verify-state"], [(os.path.join(node.dir, "yapd"), "/var/lib/yapd", "ro")],
                        self.info["uid"], self.info["gid"], check=False)
        check(r.returncode == 0, f"{node}: verify-state failed: {r.stdout}{r.stderr}")
        return ln.kv(r.stdout)

    def destroy(self):
        """Removes containers, the compose network and the node directories (keys included)."""
        self.compose("down", "--timeout", "30", "--volumes", "--remove-orphans", check_rc=False)
        shutil.rmtree(self.d, ignore_errors=True)


def check_same_chain(net, nodes, hi, lo=1):
    return livenet.check_same_chain(net, nodes, hi, lo)


def check_node_state(net, model, node, names):
    return livenet.check_node_state(net, model, node, names)


def all_accounts(net):
    return list(net.addresses.values()) + [livenet.POOL, livenet.TREASURY]
