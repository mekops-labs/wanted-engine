FROM debian:trixie-slim

LABEL maintainer="mek.xgt@gmail.com"
LABEL version="0.6.6"

# Unversioned LLVM packages track the distro default (clang/lld/lldb 19 on
# trixie). The host engine builds with gcc (build-essential) and this clang;
# the wapp toolchain is the separate bundled wasi-sdk installed below.
RUN apt-get -o Acquire::Check-Valid-Until=false -o Acquire::Check-Date=false update \
    && export DEBIAN_FRONTEND=noninteractive \
    && apt-get install -y --no-install-recommends \
    # Core build toolchain
    build-essential \
    ccache \
    clang \
    cmake \
    cmake-curses-gui \
    just \
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
    cppcheck \
    shellcheck \
    # WebAssembly tooling
    wabt \
    # Coverage reporting
    gcovr \
    # OpenSSL dev headers — platform/linux's `find_package(OpenSSL)` enables
    # SECURE_SOCKETS (TLS sockets + the real `/dev/ed25519` PlatformEd25519Verify
    # body) only when this is present; without it both silently fall back to
    # stubs (TLS off, Ed25519 verify -ENOSYS).
    libssl-dev \
    # Utilities
    ca-certificates \
    curl \
    git \
    jq \
    python-is-python3 \
    python3-venv \
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
    # apps/crypto/mbedtls fetches its source as a GitHub zip and unpacks it
    # with unzip at build time (sim TLS build).
    unzip \
    # OpenWRT SDK build prerequisites (packaging/openwrt); gawk for asort(),
    # python3-setuptools for the distutils shim.
    gawk \
    python3-setuptools \
    file \
    rsync \
    wget \
    && rm -rf /var/lib/apt/lists/*

# Route every host compile through ccache via the masquerade dir on PATH. The
# package ships gcc/clang/g++ symlinks; add cc/c++ so the NuttX kbuild (which
# invokes `cc`) is cached too. One shared cache then serves the cmake engine
# builds and the NuttX sim build alike.
RUN ln -sf ../../bin/ccache /usr/lib/ccache/cc \
    && ln -sf ../../bin/ccache /usr/lib/ccache/c++
ENV PATH="/usr/lib/ccache:${PATH}"

# semgrep (pattern-based source scan). The Debian environment is externally
# managed and semgrep's deps collide with distro packages, so install it into a
# dedicated venv and expose the entry point on PATH.
RUN python3 -m venv /opt/semgrep \
    && /opt/semgrep/bin/pip install --no-cache-dir semgrep \
    && ln -s /opt/semgrep/bin/semgrep /usr/local/bin/semgrep

# trivy (image/fs CVE + secret scan) is installed from the upstream release
# artifact for the build platform.
ARG TRIVY_VER=0.71.1
ARG TARGETARCH=amd64
RUN set -eux; \
    case "${TARGETARCH}" in \
      amd64) trivy_arch=64bit ;; \
      arm64) trivy_arch=ARM64 ;; \
      *) echo "no trivy build for TARGETARCH=${TARGETARCH}" >&2; exit 1 ;; \
    esac; \
    curl -sSfL "https://github.com/aquasecurity/trivy/releases/download/v${TRIVY_VER}/trivy_${TRIVY_VER}_Linux-${trivy_arch}.tar.gz" \
        | tar xz -C /usr/local/bin trivy

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

RUN echo "[safe]\n\tdirectory = /src" > /root/.gitconfig

ADD entry.sh /

WORKDIR /src

ENTRYPOINT [ "/bin/sh", "-C", "/entry.sh" ]
