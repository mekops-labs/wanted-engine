FROM ubuntu:22.04

LABEL maintainer="mek.xgt@gmail.com"
LABEL version="0.4.0"

RUN apt-get -o Acquire::Check-Valid-Until=false -o Acquire::Check-Date=false update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y \
    # Core build toolchain
    build-essential \
    clang \
    cmake \
    cmake-curses-gui \
    lld-14 \
    ninja-build \
    # Debuggers and runtime analysis
    gdb \
    lldb-14 \
    strace \
    valgrind \
    # Static analysis and formatting
    clang-format-14 \
    clang-tidy-14 \
    # WebAssembly tooling
    wabt \
    # Coverage reporting
    gcovr \
    # Utilities
    git \
    jq \
    python-is-python3 \
    xxd \
    # Container plumbing
    dumb-init \
    gosu \
    && rm -rf /var/lib/apt/lists/*

# Wapp (WebAssembly) toolchain: the full wasi-sdk bundle ships its own clang
# and lld plus the wasi-sysroot. wapps are built with /opt/wasi-sdk/bin/clang
# (see wasm/Makefile, wapps/*/Makefile); the host engine still builds with the
# system clang above. The bundled lld defines __heap_end, which wasi-libc's
# allocator requires — the system lld-14 does not, so wapps that use malloc
# only link against this toolchain.
ARG WASI_SDK_VER=24
ARG WASI_SDK_DIR=wasi-sdk-${WASI_SDK_VER}.0-x86_64-linux
ADD https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_VER}/${WASI_SDK_DIR}.tar.gz /tmp
RUN tar xzf /tmp/${WASI_SDK_DIR}.tar.gz -C /opt/ \
    && ln -s /opt/${WASI_SDK_DIR} /opt/wasi-sdk \
    && rm -f /tmp/${WASI_SDK_DIR}.tar.gz

ADD entry.sh /

WORKDIR /src

ENTRYPOINT [ "/bin/sh", "-C", "/entry.sh" ]
