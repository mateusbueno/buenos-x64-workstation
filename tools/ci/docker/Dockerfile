# Local Linux validation image: mirrors the CI Linux jobs (gcc, clang 20 with RealtimeSanitizer and ThreadSanitizer). git: the configure step fetches LVGL (ui/CMakeLists.txt).
# Native arm64 on Apple-silicon Macs, which is also the target ISA.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates git wget gnupg build-essential cmake ninja-build python3 libflac-dev protobuf-compiler libprotobuf-dev libsqlite3-dev libssl-dev \
    && wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key > /etc/apt/trusted.gpg.d/apt.llvm.org.asc \
    && echo "deb http://apt.llvm.org/noble/ llvm-toolchain-noble-20 main" > /etc/apt/sources.list.d/llvm20.list \
    && apt-get update && apt-get install -y --no-install-recommends clang-20 libclang-rt-20-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /work
