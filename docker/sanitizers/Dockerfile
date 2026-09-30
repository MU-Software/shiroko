# macOS 26 Apple Clang lacks LSan/MSan/HWASan/RTSan and Homebrew LLVM's ASan deadlocks in __asan_init.
# LLVM >= 20 for RTSan; HWASan needs aarch64 and lld (GNU ld mislinks tagged globals: SIGILL at startup).
FROM debian:bookworm-slim@sha256:3783cc01769c7b2b1b83a5c5ad96c815348e28ed7da68e2e3687004faa906251

ARG LLVM_VERSION=21
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates wget gnupg \
    && wget -qO- https://apt.llvm.org/llvm-snapshot.gpg.key | gpg --dearmor -o /usr/share/keyrings/llvm.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/llvm.gpg] http://apt.llvm.org/bookworm/ llvm-toolchain-bookworm-${LLVM_VERSION} main" \
        > /etc/apt/sources.list.d/llvm.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        clang-${LLVM_VERSION} \
        libclang-rt-${LLVM_VERSION}-dev \
        llvm-${LLVM_VERSION} \
        lld-${LLVM_VERSION} \
        cmake make python3 libc6-dev git curl zip unzip tar pkg-config \
    && ln -sf /usr/bin/clang-${LLVM_VERSION} /usr/bin/clang \
    && ln -sf /usr/bin/clang++-${LLVM_VERSION} /usr/bin/clang++ \
    && ln -sf /usr/bin/llvm-symbolizer-${LLVM_VERSION} /usr/bin/llvm-symbolizer \
    && ln -sf /usr/bin/ld.lld-${LLVM_VERSION} /usr/bin/ld.lld \
    && rm -rf /var/lib/apt/lists/*

ARG VCPKG_BASELINE=e5a1490e1409d175932ef6014519e9ae149ddb7c
RUN git clone https://github.com/microsoft/vcpkg /opt/vcpkg \
    && git -C /opt/vcpkg checkout -q ${VCPKG_BASELINE} \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && chmod -R a+rwX /opt/vcpkg && git config --system --add safe.directory '*'
# Containers run as the host user (make passes --user): vcpkg writes its buildtrees under /opt/vcpkg.

ENV CC=clang CXX=clang++ VCPKG_ROOT=/opt/vcpkg \
    ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer \
    MSAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer
# The library build bakes fonts with the uv-managed tools; the environment lives outside the mounted repo.
COPY --from=ghcr.io/astral-sh/uv:0.11.6 /uv /usr/local/bin/uv
ENV UV_PROJECT_ENVIRONMENT=/opt/venv UV_PYTHON_INSTALL_DIR=/opt/python UV_LINK_MODE=copy
COPY --from=tools pyproject.toml uv.lock .python-version /opt/tools/
RUN cd /opt/tools && uv sync --locked --group tools
WORKDIR /src
