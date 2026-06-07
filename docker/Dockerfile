FROM debian:trixie-slim

LABEL maintainer="mek.xgt@gmail.com"
LABEL version="0.5.0"

# Unversioned LLVM packages track the distro default (clang/lld/lldb 19 on
# trixie). The host engine builds with gcc (build-essential) and this clang;
# the wapp toolchain is the separate bundled wasi-sdk installed below.
RUN apt-get -o Acquire::Check-Valid-Until=false -o Acquire::Check-Date=false update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends \
    # Core build toolchain
    build-essential \
    clang \
    cmake \
    cmake-curses-gui \
    lld \
    ninja-build \
    # Debuggers and runtime analysis
    gdb \
    lldb \
    strace \
    valgrind \
    # Static analysis and formatting
    clang-format \
    clang-tidy \
    # WebAssembly tooling
    wabt \
    # Coverage reporting
    gcovr \
    # Utilities
    ca-certificates \
    curl \
    git \
    jq \
    python-is-python3 \
    xxd \
    # Container plumbing
    dumb-init \
    gosu \
    # NuttX simulator build prerequisites (configure + build of sim:nsh).
    # kconfig-frontends provides kconfig-conf/kconfig-tweak; the sim target
    # compiles with the host gcc above, so no cross-compiler is needed.
    kconfig-frontends \
    genromfs \
    gperf \
    flex \
    bison \
    gettext \
    libelf-dev \
    && rm -rf /var/lib/apt/lists/*

# Wapp (WebAssembly) toolchain: the full wasi-sdk bundle ships its own clang
# and lld plus the wasi-sysroot. wapps are built with /opt/wasi-sdk/bin/clang
# (see wasm/Makefile, wapps/*/Makefile); the host engine builds with the system
# clang above. The bundled lld defines __heap_end, which wasi-libc's allocator
# requires, so wapps that use malloc link against this toolchain.
#
# The bundle ships a host-arch clang binary, so the right one is selected per
# build platform. wasi-sdk publishes x86_64 and arm64 Linux builds only.
ARG WASI_SDK_VER=24
ARG TARGETARCH=amd64
RUN set -eux; \
    case "${TARGETARCH}" in \
      amd64) wasi_arch=x86_64 ;; \
      arm64) wasi_arch=arm64 ;; \
      *) echo "no wasi-sdk build for TARGETARCH=${TARGETARCH}" >&2; exit 1 ;; \
    esac; \
    dir="wasi-sdk-${WASI_SDK_VER}.0-${wasi_arch}-linux"; \
    curl -fsSL "https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_VER}/${dir}.tar.gz" \
        -o /tmp/wasi-sdk.tar.gz; \
    tar xzf /tmp/wasi-sdk.tar.gz -C /opt/; \
    ln -s "/opt/${dir}" /opt/wasi-sdk; \
    rm -f /tmp/wasi-sdk.tar.gz

ADD entry.sh /

WORKDIR /src

ENTRYPOINT [ "/bin/sh", "-C", "/entry.sh" ]
