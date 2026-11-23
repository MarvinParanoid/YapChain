# SPDX-License-Identifier: Apache-2.0
"""Helpers for tests that drive the real `yap` binary as a subprocess (no library calls)."""
import os
import subprocess
import tempfile


class Cli:
    def __init__(self, yap, home, password_file=None):
        self.yap, self.home, self.password_file = yap, home, password_file

    def run(self, *args, password_file=None, stdin=None, expect=0):
        cmd = [self.yap, "--home", self.home]
        pw = password_file or self.password_file
        if pw:
            cmd += ["--password-file", pw]
        cmd += [str(a) for a in args]
        p = subprocess.run(cmd, input=stdin, capture_output=True, text=True, timeout=600)
        if expect is not None and p.returncode != expect:
            raise AssertionError(f"{' '.join(cmd)}\nexit {p.returncode}, expected {expect}\n"
                                 f"stdout:\n{p.stdout}\nstderr:\n{p.stderr}")
        return p

    def kv(self, *args, **kw):
        """Runs a command and parses its `key: value` output."""
        out = {}
        for line in self.run(*args, **kw).stdout.splitlines():
            if ": " in line:
                k, v = line.split(": ", 1)
                out[k] = v
        return out


def password_file(directory, text):
    path = os.path.join(directory, "pw-" + str(abs(hash(text))))
    with open(path, "w") as f:
        f.write(text + "\n")
    os.chmod(path, 0o600)
    return path


def tmpdir():
    return tempfile.mkdtemp(prefix="yap-cli-")


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
