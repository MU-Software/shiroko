# x86_64 GCC: argument evaluation order and warnings differ from Clang.
FROM gcc:13@sha256:16ae525998c94df36a116c191524256b1d46e72d7a0e9aaf6c153455e40eb5b8
RUN apt-get update && apt-get install -y --no-install-recommends cmake git curl zip unzip tar pkg-config ninja-build \
    && rm -rf /var/lib/apt/lists/*
ARG VCPKG_BASELINE=e5a1490e1409d175932ef6014519e9ae149ddb7c
RUN git clone https://github.com/microsoft/vcpkg /opt/vcpkg \
    && git -C /opt/vcpkg checkout -q ${VCPKG_BASELINE} \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics \
    && chmod -R a+rwX /opt/vcpkg && git config --system --add safe.directory '*'
# Containers run as the host user (make passes --user): vcpkg writes its buildtrees under /opt/vcpkg.
ENV VCPKG_ROOT=/opt/vcpkg
# The library build bakes fonts with the uv-managed tools; the environment lives outside the mounted repo.
COPY --from=ghcr.io/astral-sh/uv:0.11.6 /uv /usr/local/bin/uv
ENV UV_PROJECT_ENVIRONMENT=/opt/venv UV_PYTHON_INSTALL_DIR=/opt/python UV_LINK_MODE=copy
COPY --from=tools pyproject.toml uv.lock .python-version /opt/tools/
RUN cd /opt/tools && uv sync --locked --group tools
WORKDIR /src
