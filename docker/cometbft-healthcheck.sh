#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
#
# Container healthcheck of CometBFT (M2.7): the RPC server inside the container answers GET /health with
# HTTP 200. This is LIVENESS/RPC readiness only. It does not require block production, quorum, peers or
# being caught up: a node without quorum or one that is catching up is alive and healthy (see
# `yapnode.py status` for synchronisation, consensus progress and peers). bash /dev/tcp is used so that
# the image needs no HTTP client.
set -u
port="${YAP_RPC_HEALTH_PORT:-26657}"
exec 3<>"/dev/tcp/127.0.0.1/${port}" || exit 1
printf 'GET /health HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n' >&3
IFS= read -r -t 4 status <&3 || exit 1
exec 3<&-
case "$status" in
    HTTP/1.?" 200"*) exit 0 ;;
esac
echo "cometbft RPC /health: ${status}" >&2
exit 1
