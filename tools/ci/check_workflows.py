#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Local checks of .github/workflows (no network, PyYAML only).

    check_workflows.py [repo root]

- every file parses as YAML and has `on`, `permissions` and `jobs`;
- workflow permissions are read-only (no `write` anywhere);
- every job has `timeout-minutes`; no `continue-on-error` anywhere;
- every `uses:` is pinned to a full 40-hex commit SHA;
- ci.yml: the build/test matrix expands (GitHub `include` semantics) to exactly 12 jobs —
  {x86_64, aarch64} x {gcc, clang} x {debug, release, asan} — each with the right runner, and the
  reduced long-test scale applies to aarch64/gcc/asan only; the fuzz job runs every fuzz target;
  the live jobs listed in REQUIRED_LIVE_JOBS check that their tests are registered.
Prints the expanded matrix and exits non-zero on any finding.
"""
import itertools
import os
import re
import sys

try:
    import yaml
except ImportError:  # reported as "skipped" by CTest (SKIP_RETURN_CODE 77), never as a pass
    print("PyYAML is not installed: workflow checks skipped", file=sys.stderr)
    sys.exit(77)

FINDINGS = []
REQUIRED_LIVE_JOBS = ()  # the live / tsan jobs that must check their test registration


def finding(msg):
    FINDINGS.append(msg)
    print(f"FINDING: {msg}", file=sys.stderr)


def expand(matrix):
    """GitHub Actions matrix expansion: the product of the axes, then each `include` entry is added to
    every combination whose ORIGINAL axis values it does not contradict (without overwriting them);
    an entry that matches no combination becomes a new one. `exclude` is not used here."""
    axes = {k: v for k, v in matrix.items() if k not in ("include", "exclude")}
    combos = [dict(zip(axes, vals)) for vals in itertools.product(*axes.values())]
    originals = [dict(c) for c in combos]
    for inc in matrix.get("include", []):
        matched = False
        for c, orig in zip(combos, originals):
            if all(orig.get(k, v) == v for k, v in inc.items() if k in orig):
                matched = True
                for k, v in inc.items():
                    if k not in orig:
                        c[k] = v
        if not matched:
            combos.append(dict(inc))
            originals.append(dict(inc))
    return combos


def walk(node, path=""):
    if isinstance(node, dict):
        for k, v in node.items():
            yield f"{path}.{k}", k, v
            yield from walk(v, f"{path}.{k}")
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from walk(v, f"{path}[{i}]")


def main(root):
    wdir = os.path.join(root, ".github", "workflows")
    files = sorted(f for f in os.listdir(wdir) if f.endswith((".yml", ".yaml")))
    docs = {}
    for f in files:
        with open(os.path.join(wdir, f)) as fh:
            doc = yaml.load(fh, Loader=yaml.BaseLoader)  # strings only: `on` stays a key
        docs[f] = doc
        for key in ("on", "permissions", "jobs"):
            if key not in doc:
                finding(f"{f}: missing top-level '{key}'")
        perms = doc.get("permissions", {})
        if perms != {"contents": "read"}:
            finding(f"{f}: workflow permissions {perms} (expected contents: read)")
        for path, k, v in walk(doc):
            if k == "continue-on-error":
                finding(f"{f}{path}: continue-on-error")
            if k == "permissions" and path != ".permissions" and "write" in str(v):
                finding(f"{f}{path}: write permission")
            if k == "uses" and not re.fullmatch(r"[\w.-]+/[\w.-]+@[0-9a-f]{40}", str(v)):
                finding(f"{f}{path}: '{v}' is not pinned to a full commit SHA")
        for name, job in doc.get("jobs", {}).items():
            if "timeout-minutes" not in job:
                finding(f"{f}: job '{name}' has no timeout-minutes")
        print(f"{f}: {len(doc.get('jobs', {}))} job(s), parsed")

    ci = docs.get("ci.yml")
    if ci is None:
        finding("ci.yml missing")
        return report()
    combos = expand(ci["jobs"]["build-test"]["strategy"]["matrix"])
    print(f"ci.yml build-test: {len(combos)} jobs")
    for c in combos:
        print("   ", {k: c[k] for k in sorted(c)})
    want = {(a, c, p) for a in ("x86_64", "aarch64") for c in ("gcc", "clang") for p in ("debug", "release", "asan")}
    got = {(c.get("arch"), c.get("compiler"), c.get("preset")) for c in combos}
    if len(combos) != 12 or got != want:
        finding(f"build-test matrix: {len(combos)} jobs, missing {want - got}, unexpected {got - want}")
    runners = {"x86_64": "ubuntu-24.04", "aarch64": "ubuntu-24.04-arm"}
    for c in combos:
        if c.get("runner") != runners.get(c.get("arch")):
            finding(f"build-test {c}: runner {c.get('runner')}")
        scaled = "long_test_scale" in c
        target = (c.get("arch"), c.get("compiler"), c.get("preset")) == ("aarch64", "gcc", "asan")
        if scaled != target:
            finding(f"build-test {c}: the reduced scale must apply to aarch64/gcc/asan only")
    fuzz = " ".join(s.get("run", "") for s in ci["jobs"]["fuzz"]["steps"])
    for t in ("ycs", "bech32m", "tx", "devblock", "abci", "rpc"):
        if not re.search(rf"\b{t}\b", fuzz):
            finding(f"fuzz job does not run fuzz_{t}")
    for job in REQUIRED_LIVE_JOBS:
        if job not in ci["jobs"]:
            finding(f"ci.yml: no '{job}' job")
            continue
        steps = " ".join(s.get("run", "") for s in ci["jobs"][job]["steps"])
        if "test \"$n\" -eq 3" not in steps:
            finding(f"ci.yml {job}: the registration of the live tests is not checked")
    return report()


def report():
    if FINDINGS:
        print(f"{len(FINDINGS)} finding(s)", file=sys.stderr)
        return 1
    print("workflows: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main(os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")))
