# Story 6.12: RAUC host tool for the bundle/signature tests . Built from the pinned release tarball (same version and sha256 as
# meta-rauc's rauc-1.15.2.inc at the pinned wrynose commit), because Ubuntu 24.04's rauc package is older than the >= 1.15.2 the layer uses.
# Only the host-side bundle tooling is built (no service, no network/streaming, no bootloader access needed).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
ARG RAUC_VERSION=1.15.2
ARG RAUC_SHA256=127a24cde208c65b837ae978c695a00730f1094ee8b6c7d48cf58ef846eae340
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates curl xz-utils meson ninja-build gcc pkg-config libglib2.0-dev libssl-dev libjson-glib-dev \
      squashfs-tools openssl python3 e2fsprogs \
    && cd /tmp && curl -fsSL -o rauc.tar.xz "https://github.com/rauc/rauc/releases/download/v${RAUC_VERSION}/rauc-${RAUC_VERSION}.tar.xz" \
    && echo "${RAUC_SHA256}  rauc.tar.xz" | sha256sum -c - \
    && tar xf rauc.tar.xz && cd "rauc-${RAUC_VERSION}" \
    && meson setup build --prefix=/usr/local -Dservice=false -Dnetwork=false -Dstreaming=false -Djson=enabled -Dgpt=disabled -Dtests=false -Dmanpages=false \
    && ninja -C build install \
    && cd / && rm -rf /tmp/rauc* \
    && apt-get purge -y --auto-remove curl xz-utils meson ninja-build gcc \
    && rm -rf /var/lib/apt/lists/*
RUN useradd -m -u 1501 tester
USER tester
WORKDIR /home/tester
