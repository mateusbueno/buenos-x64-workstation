# Story 1.4: containerised Yocto parse/policy validation (no image build). Runs bitbake as a non-root user (bitbake refuses root).
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
RUN apt-get update && apt-get install -y --no-install-recommends \
      gawk wget git diffstat unzip texinfo gcc build-essential chrpath socat cpio python3 python3-pip python3-pexpect \
      python3-venv xz-utils debianutils iputils-ping python3-git python3-jinja2 python3-subunit zstd liblz4-tool file \
      rsync locales libacl1 lz4 ca-certificates curl sudo \
    && locale-gen en_US.UTF-8 \
    && rm -rf /var/lib/apt/lists/*
# kas pinned: layer checkout and bitbake environment from the project file
RUN python3 -m venv /opt/kas && /opt/kas/bin/pip install --no-cache-dir kas==4.7 && ln -s /opt/kas/bin/kas /usr/local/bin/kas
RUN useradd -m -u 1500 builder
USER builder
WORKDIR /home/builder
