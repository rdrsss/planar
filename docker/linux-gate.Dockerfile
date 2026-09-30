# syntax=docker/dockerfile:1.7
#
# Planar Linux build-and-test gate (task 7094).
#
# Builds the `debug` preset and runs the whole ctest suite on Debian trixie,
# so Linux-only code (close_range / /proc/self/fd in src/lib/process,
# pipe2(O_CLOEXEC) in runner.cpp, /proc/<pid>/fd in queue_run.t.cpp) is
# compiled and executed somewhere other than macOS.
#
# Run it with `make linux-gate` (see docs/testing.md); do not hand-roll the
# flags. Stages:
#
#   toolchain  debian:trixie-slim + apt.llvm.org LLVM ${LLVM_MAJOR} (clang,
#              libc++ with modules manifest, libc++abi) + Kitware CMake
#              ${CMAKE_VERSION} (pinned by SHA-256) + ninja, git, python3,
#              sqlite3, libtbb-dev, libssl-dev (vendored libcurl's TLS on Linux),
#              patch (CPM's PATCHES keyword for Mt-KaHyPar).
#   run        copies the source, builds and tests. It NEVER fails the image
#              build: it writes its logs and exit status under /out so a
#              failure is reported with its evidence instead of vanishing
#              into BuildKit's 2 MiB step-log cap.
#   gate       FROM scratch, holds only /out. `make linux-gate` exports it
#              with `--output` and turns status.txt into the exit code.
#
# external/ (Centurion, a PRIVATE first-party repo fetched at configure time)
# is NOT copied into a layer and no token enters the image. The Makefile
# passes the already-populated host directory as the named build context
# `external` (--build-context external=<dir>/external/centurion/<tag>, just the pinned
# tag: the directory also holds other tags and is ~1 GB) and it is
# bind-mounted (writes discarded) at /src/external/centurion/<tag> for the one
# RUN that configures. If it is ever necessary to fetch inside the build,
# use a BuildKit secret (--secret id=gh,env=GITHUB_TOKEN); never ARG/ENV.
#
# The build tree lives in a BuildKit cache mount, not in an image layer, so
# repeat runs are incremental and the image stays small. Reclaim the disk
# with `make linux-gate-prune` (docker builder prune -f).

ARG LLVM_MAJOR=23
ARG CMAKE_VERSION=4.4.2
ARG CMAKE_SHA256_AARCH64=9ca1aadb4451c5dcbdc67f9b4aff42dab52abbaebd8db9e2900026502dbed671
ARG CMAKE_SHA256_X86_64=3ada9a3f5d8a85413579bdd0ea6aa8e8da86efdd6d15c91a1afa517f2021956c

FROM debian:trixie-slim AS toolchain
ARG LLVM_MAJOR
ARG CMAKE_VERSION
ARG CMAKE_SHA256_AARCH64
ARG CMAKE_SHA256_X86_64
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl gnupg git ninja-build make pkg-config \
      python3 python3-venv sqlite3 libtbb-dev libssl-dev zlib1g-dev patch procps xz-utils \
 && rm -rf /var/lib/apt/lists/*

# CMake >= 4.3 is not in Debian; take Kitware's release tarball, verified.
RUN set -eu; \
    case "$(uname -m)" in \
      aarch64) arch=aarch64; sha="${CMAKE_SHA256_AARCH64}" ;; \
      x86_64)  arch=x86_64;  sha="${CMAKE_SHA256_X86_64}" ;; \
      *) echo "unsupported arch $(uname -m)"; exit 1 ;; \
    esac; \
    curl -fsSL -o /tmp/cmake.tar.gz \
      "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${arch}.tar.gz"; \
    echo "${sha}  /tmp/cmake.tar.gz" | sha256sum -c -; \
    tar -xzf /tmp/cmake.tar.gz -C /opt; \
    ln -s "/opt/cmake-${CMAKE_VERSION}-linux-${arch}/bin/cmake" /usr/local/bin/cmake; \
    ln -s "/opt/cmake-${CMAKE_VERSION}-linux-${arch}/bin/ctest" /usr/local/bin/ctest; \
    rm -f /tmp/cmake.tar.gz

# The pinned LLVM major from apt.llvm.org (docs/toolchain-parity.md).
RUN set -eu; \
    curl -fsSL https://apt.llvm.org/llvm-snapshot.gpg.key -o /usr/share/keyrings/llvm.asc; \
    . /etc/os-release; \
    echo "deb [signed-by=/usr/share/keyrings/llvm.asc] https://apt.llvm.org/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-${LLVM_MAJOR} main" \
      > /etc/apt/sources.list.d/llvm.list; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      clang-${LLVM_MAJOR} lld-${LLVM_MAJOR} libc++-${LLVM_MAJOR}-dev libc++abi-${LLVM_MAJOR}-dev; \
    rm -rf /var/lib/apt/lists/*

ENV PLANAR_LLVM_PREFIX=/usr/lib/llvm-${LLVM_MAJOR}

# Fail the toolchain stage, not a 20-minute configure, if the pin is unusable.
RUN set -eu; \
    cmake --version | head -1; \
    "${PLANAR_LLVM_PREFIX}/bin/clang++" --version | head -1; \
    test -d "${PLANAR_LLVM_PREFIX}/include/c++/v1"; \
    find "${PLANAR_LLVM_PREFIX}/lib" -name 'libc++.modules.json' | grep -q .

FROM toolchain AS run
# Sibling-lane hosts are shared; the Makefile passes a bounded job count.
ARG JOBS=4
ARG CENTURION_TAG
ARG CTEST_ARGS=
WORKDIR /src
COPY . /src

# The whole gate is one RUN so the build cache mount is visible to every step.
# It always exits 0 and records the verdict: see the header.
RUN --mount=type=bind,from=external,target=/src/external/centurion/${CENTURION_TAG},rw \
    --mount=type=cache,target=/src/build/debug,id=planar-linux-gate-build \
    mkdir -p /out; \
    rc=0; \
    { cmake --preset debug >/out/configure.log 2>&1 || rc=$?; \
      echo "configure_exit=$rc" >>/out/status.txt; } ; \
    if [ "$rc" = 0 ]; then \
      cmake --build build/debug -j"${JOBS}" >/out/build.log 2>&1 || rc=$?; \
      echo "build_exit=$rc" >>/out/status.txt; \
    fi; \
    if [ "$rc" = 0 ]; then \
      ctest_rc=0; \
      ctest --test-dir build/debug -j"${JOBS}" --output-on-failure ${CTEST_ARGS} >/out/ctest.log 2>&1 || ctest_rc=$?; \
      echo "ctest_exit=$ctest_rc" >>/out/status.txt; \
      rc=$ctest_rc; \
    fi; \
    echo "status=$rc" >>/out/status.txt; \
    exit 0

FROM scratch AS gate
COPY --from=run /out /
