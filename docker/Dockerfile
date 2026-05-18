FROM ubuntu:22.04

LABEL maintainer="mek.xgt@gmail.com"
LABEL version="0.3.0"

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

# v16-v25 use the libclang_rt.builtins-wasm32-wasi-* naming convention.
# v26+ switched to libclang_rt-*.tar.gz and bundle their own clang — upgrading
# beyond v25 requires switching to the full wasi-sdk-*.tar.gz bundle and
# updating wasm/Makefile to use /opt/wasi-sdk/bin/clang.
ARG WASI_SDK_VER=24
ARG LIBCLANG_RT_FILE=libclang_rt.builtins-wasm32-wasi-${WASI_SDK_VER}.0.tar.gz
ADD https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_VER}/${LIBCLANG_RT_FILE} /tmp
RUN tar xzf /tmp/${LIBCLANG_RT_FILE} -C /usr/lib/clang/14/ && rm -f ${LIBCLANG_RT_FILE}

ARG WASI_SYSROOT_FILE=wasi-sysroot-${WASI_SDK_VER}.0.tar.gz
ADD https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_VER}/${WASI_SYSROOT_FILE} /tmp
RUN tar xzf /tmp/${WASI_SYSROOT_FILE} -C /usr/share/ && rm -f ${WASI_SYSROOT_FILE}

ADD entry.sh /

WORKDIR /src

ENTRYPOINT [ "/bin/sh", "-C", "/entry.sh" ]
