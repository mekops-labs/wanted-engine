FROM ubuntu:22.04

LABEL maintainer="mek.xgt@gmail.com"
LABEL version="0.2.0"

RUN apt-get -o Acquire::Check-Valid-Until=false -o Acquire::Check-Date=false update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y \
    build-essential \
    clang \
    cmake \
    dumb-init \
    gcovr \
    git \
    gosu \
    lld-14 \
    ninja-build \
    python-is-python3 \
    xxd \
    && rm -rf /var/lib/apt/lists/*

ARG WASI_SDK_VER=16
ARG LIBCLANG_RT_FILE=libclang_rt.builtins-wasm32-wasi-${WASI_SDK_VER}.0.tar.gz
ADD https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_VER}/${LIBCLANG_RT_FILE} /tmp
RUN tar xzf /tmp/${LIBCLANG_RT_FILE} -C /usr/lib/clang/14/ && rm -f ${LIBCLANG_RT_FILE}

ARG WASI_SYSROOT_FILE=wasi-sysroot-${WASI_SDK_VER}.0.tar.gz
ADD https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_VER}/${WASI_SYSROOT_FILE} /tmp
RUN tar xzf /tmp/${WASI_SYSROOT_FILE} -C /usr/share/ && rm -f ${WASI_SYSROOT_FILE}

ADD entry.sh /

WORKDIR /src

ENTRYPOINT [ "/bin/sh", "-C", "/entry.sh" ]