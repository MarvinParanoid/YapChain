#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Entrypoint of the CometBFT container (M2.7, ADR-0031).
#
#   start (default)  check the node home, wait until this node's yapd answers on the ABCI socket
#                    (readiness, no fixed sleep), then exec `cometbft start` (it becomes PID 1 and handles
#                    SIGTERM itself: graceful shutdown, nothing half-written)
#   <anything else>  passed to cometbft unchanged (e.g. `show-node-id`, `version`)
#
# Environment: CMTHOME (default /var/lib/cometbft), YAP_ABCI_SOCKET (default /run/yap/abci/yapd.sock),
# YAP_ABCI_WAIT_MS (default 120000). It never creates, modifies or deletes keys, genesis or data: an
# uninitialised home is an error (initialise it with scripts/ops/yapnode.py).
set -eu

home="${CMTHOME:-/var/lib/cometbft}"
socket="${YAP_ABCI_SOCKET:-/run/yap/abci/yapd.sock}"
wait_ms="${YAP_ABCI_WAIT_MS:-120000}"

die() {
    echo "cometbft-entrypoint: $*" >&2
    exit 1
}

if [ "${1:-start}" != "start" ]; then
    exec /usr/local/bin/cometbft "$@"
fi

for f in config/config.toml config/genesis.json config/node_key.json config/priv_validator_key.json; do
    [ -f "$home/$f" ] || die "$home/$f is missing: the node home is not initialised (scripts/ops/yapnode.py init)"
    [ -r "$home/$f" ] || die "$home/$f is not readable by uid $(id -u)"
done
[ -d "$home/data" ] && [ -w "$home/data" ] || die "$home/data is missing or not writable by uid $(id -u)"
# The genesis must be the one the node was configured with (yapnode.py configure records its SHA-256).
# CometBFT alone would keep the genesis stored in its database and silently ignore a replaced file.
if [ -f "$home/config/genesis.json.sha256" ]; then
    want="$(tr -d ' \n' < "$home/config/genesis.json.sha256")"
    got="$(sha256sum "$home/config/genesis.json" | cut -d' ' -f1)"
    [ "$got" = "$want" ] || die "config/genesis.json has SHA-256 $got but this node was configured with $want; refusing to start (restore the original genesis; a genesis is never replaced)"
else
    echo "cometbft-entrypoint: warning: no config/genesis.json.sha256 (node not configured by yapnode.py)" >&2
fi
# The application must be this node's yapd on the shared socket, never a TCP address.
grep -Eq "^proxy_app = \"unix://${socket}\"\$" "$home/config/config.toml" \
    || die "config.toml proxy_app is not unix://${socket}"

echo "cometbft-entrypoint: waiting for yapd on ${socket} (up to ${wait_ms} ms)" >&2
/usr/local/bin/yap-abci-probe --socket "$socket" --wait-ms "$wait_ms" \
    || die "yapd does not answer on ${socket}; not starting CometBFT"
exec /usr/local/bin/cometbft start --home "$home"
