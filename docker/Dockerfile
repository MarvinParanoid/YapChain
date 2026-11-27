# SPDX-License-Identifier: Apache-2.0
#
# YapChain container images (M2.7, ADR-0031). One Dockerfile, two final targets:
#
#   docker build -f docker/Dockerfile --target yapchain -t yapchain:dev .   # yapd + yap + yap-abci-probe
#   docker build -f docker/Dockerfile --target cometbft -t yapchain-cometbft:dev .  # CometBFT v0.40.0 + probe
#
# (or `python3 -I scripts/ops/yapnode.py build`, which passes the build arguments below and records the
# image digests). Build context = repository root, filtered by the whitelist in .dockerignore.
#
# Supply chain:
#   - base images are pinned by digest (UBUNTU_IMAGE, GO_IMAGE);
#   - Ubuntu packages come from the Ubuntu snapshot archive at UBUNTU_SNAPSHOT (a fixed point in time,
#     signatures verified by apt), so the same build arguments give the same package versions;
#   - every third-party source archive is fetched by tools/deps/fetch_deps.py (pinned SHA-256, verified
#     before use, bounded retries) and the C++ build runs OFFLINE from that directory (YAP_DEPS_DIR);
#   - CometBFT is built from the pinned tag archive exactly as localnet.py does (ADR-0027): SHA-256 of the
#     archive, `go mod verify`, -mod=readonly, -trimpath, commit stamped with -ldflags;
#   - runtime images contain no compiler, no package manager caches, no keys and no credentials, and run as
#     the non-root user 10001:10001 (the same numeric id in both images: the ABCI socket is 0600).

ARG UBUNTU_IMAGE=ubuntu:24.04@sha256:562111b88caa79c71391c0c4d3c2114a7d5c35915c20561fd893cb1e07929e6b
ARG GO_IMAGE=golang:1.27.4-bookworm@sha256:ce59b41415e2524ae99926fe1f37ce48ba8c194dcbd2a56a65436cdcefa2bd34
ARG UBUNTU_SNAPSHOT=20261126T000000Z

# ---------------------------------------------------------------- base: snapshot apt sources
FROM ${UBUNTU_IMAGE} AS ubuntu-base
ARG UBUNTU_SNAPSHOT
ENV DEBIAN_FRONTEND=noninteractive LC_ALL=C.UTF-8
# The snapshot service is HTTPS-only, so ca-certificates is bootstrapped from the release pocket (packages
# are verified by apt signatures either way). Then every source (archive, security and, on arm64, ports —
# the snapshot of the main archive carries all architectures) points at the dated snapshot, and
# ca-certificates is pinned to the snapshot's own version, so the package set depends only on
# UBUNTU_SNAPSHOT.
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates \
 && rm -rf /var/lib/apt/lists/* \
 && snap="https://snapshot.ubuntu.com/ubuntu/${UBUNTU_SNAPSHOT}/" \
 && sed -i "s|http://archive.ubuntu.com/ubuntu/|${snap}|; s|http://security.ubuntu.com/ubuntu/|${snap}|; s|http://ports.ubuntu.com/ubuntu-ports/|${snap}|" \
      /etc/apt/sources.list.d/ubuntu.sources \
 && ! grep -E '^URIs:' /etc/apt/sources.list.d/ubuntu.sources | grep -v "${snap}" \
 && apt-get update \
 && v="$(apt-cache policy ca-certificates | awk '/Candidate:/ {print $2}')" \
 && apt-get install -y --no-install-recommends --allow-downgrades "ca-certificates=${v}" \
 && rm -rf /var/lib/apt/lists/*

# ---------------------------------------------------------------- deps: pinned source archives
FROM ubuntu-base AS deps
RUN apt-get update \
 && apt-get install -y --no-install-recommends python3 \
 && rm -rf /var/lib/apt/lists/*
COPY tools/deps/fetch_deps.py /opt/yap/fetch_deps.py
RUN python3 -I /opt/yap/fetch_deps.py --dest /deps --group build \
 && python3 -I /opt/yap/fetch_deps.py --dest /deps --group cometbft \
 && rm -f /deps/.fetch-deps.lock \
 && cd /deps && sha256sum * > /deps/SHA256SUMS

# ---------------------------------------------------------------- build: C++23 Release, offline
FROM ubuntu-base AS build
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++-14 cmake ninja-build make python3 libcurl4-openssl-dev \
 && rm -rf /var/lib/apt/lists/* \
 && dpkg-query -W -f '${Package} ${Version}\n' g++-14 cmake ninja-build libcurl4-openssl-dev > /opt/build-packages.txt
COPY --from=deps /deps /deps
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json LICENSE ./
COPY cmake cmake
COPY third_party third_party
COPY src src
COPY tools/ci/check_cxx_standard.py tools/ci/check_cxx_standard.py
# The release preset (Release, -Werror, the project's warning flags and the GCC 14 ICE workaround from
# cmake/Warnings.cmake); tests are not built into images, they run in CI and the container tests.
RUN cmake --preset release -DYAP_BUILD_TESTS=OFF -DYAP_DEPS_DIR=/deps \
      -DCMAKE_C_COMPILER=gcc-14 -DCMAKE_CXX_COMPILER=g++-14 \
 && cmake --build --preset release --target yap yapd yap_abci_probe \
 && python3 -I tools/ci/check_cxx_standard.py build/release . \
 && mkdir -p /out \
 && cp build/release/src/cli/yap build/release/src/node/yapd build/release/src/node/yap-abci-probe /out/ \
 && strip --strip-unneeded /out/yap /out/yapd /out/yap-abci-probe \
 && (cd /out && sha256sum yap yapd yap-abci-probe > SHA256SUMS)

# ---------------------------------------------------------------- cometbft-build: ADR-0027 procedure
FROM ${GO_IMAGE} AS go-toolchain
FROM ubuntu-base AS cometbft-build
RUN apt-get update \
 && apt-get install -y --no-install-recommends python3 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=go-toolchain /usr/local/go /usr/local/go
ENV PATH=/usr/local/go/bin:${PATH} GOTOOLCHAIN=local GOPATH=/tmp/go GOCACHE=/tmp/go-cache
COPY --from=deps /deps /deps
COPY tools/deps/fetch_deps.py /src/tools/deps/fetch_deps.py
COPY scripts/localnet/localnet.py /src/scripts/localnet/localnet.py
RUN python3 -I /src/scripts/localnet/localnet.py build-cometbft --deps-dir /deps --out /out \
 && cat /out/cometbft.build-info

# ---------------------------------------------------------------- runtime base
FROM ubuntu-base AS runtime-base
RUN groupadd --gid 10001 yap \
 && useradd --uid 10001 --gid 10001 --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin yap \
 && install -d -m 0755 /run/yap \
 && install -d -o 10001 -g 10001 -m 0700 /run/yap/abci

# ---------------------------------------------------------------- yapchain: yapd + yap
FROM runtime-base AS yapchain
# libcurl for the network commands of yap (yapd itself does not use it); nothing else is added.
RUN apt-get update \
 && apt-get install -y --no-install-recommends libcurl4t64 \
 && rm -rf /var/lib/apt/lists/* /var/cache/apt/* /var/log/apt /var/log/dpkg.log \
 && install -d -o 10001 -g 10001 -m 0700 /var/lib/yapd
COPY --from=build /out/yap /out/yapd /out/yap-abci-probe /usr/local/bin/
COPY --from=build /out/SHA256SUMS /usr/local/share/yapchain/SHA256SUMS
COPY --from=build /opt/build-packages.txt /usr/local/share/yapchain/build-packages.txt
COPY --from=deps /deps/SHA256SUMS /usr/local/share/yapchain/deps-SHA256SUMS
COPY LICENSE /usr/local/share/yapchain/LICENSE
RUN dpkg-query -W -f '${Package} ${Version}\n' > /usr/local/share/yapchain/runtime-packages.txt
USER 10001:10001
ENV YAPD_HOME=/var/lib/yapd
# Exits 0 on SIGTERM/SIGINT after the in-flight request; never forks, so it can be PID 1.
ENTRYPOINT ["/usr/local/bin/yapd"]
CMD ["--home", "/var/lib/yapd", "--socket", "/run/yap/abci/yapd.sock"]
HEALTHCHECK --interval=10s --timeout=5s --start-period=60s --retries=3 \
  CMD ["/usr/local/bin/yap-abci-probe", "--socket", "/run/yap/abci/yapd.sock", "--timeout-ms", "3000", "--quiet"]
STOPSIGNAL SIGTERM

# ---------------------------------------------------------------- cometbft
FROM runtime-base AS cometbft
COPY --from=cometbft-build /out/cometbft /usr/local/bin/cometbft
COPY --from=cometbft-build /out/cometbft.build-info /usr/local/share/yapchain/cometbft.build-info
COPY --from=build /out/yap-abci-probe /usr/local/bin/yap-abci-probe
COPY docker/cometbft-entrypoint.sh /usr/local/bin/cometbft-entrypoint
COPY docker/cometbft-healthcheck.sh /usr/local/bin/cometbft-healthcheck
RUN chmod 0755 /usr/local/bin/cometbft-entrypoint /usr/local/bin/cometbft-healthcheck \
 && install -d -o 10001 -g 10001 -m 0700 /var/lib/cometbft \
 && dpkg-query -W -f '${Package} ${Version}\n' > /usr/local/share/yapchain/runtime-packages.txt
USER 10001:10001
ENV CMTHOME=/var/lib/cometbft
ENTRYPOINT ["/usr/local/bin/cometbft-entrypoint"]
CMD ["start"]
HEALTHCHECK --interval=10s --timeout=5s --start-period=120s --retries=3 CMD ["/usr/local/bin/cometbft-healthcheck"]
STOPSIGNAL SIGTERM
