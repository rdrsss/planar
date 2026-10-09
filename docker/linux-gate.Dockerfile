# syntax=docker/dockerfile:1.7
#
# Planar Linux build-and-test gate (task 7094).
#
# Builds the `debug` preset and runs the whole ctest suite and queue observer
# Python checks on Debian trixie,
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
#              wget (the bootstrap.release test's fallback case), sqlite3,
#              libssl-dev (vendored libcurl's TLS on Linux).
#   run        copies the source, builds and tests. It NEVER fails the image
#              build: it writes its logs and exit status under /out so a
#              failure is reported with its evidence instead of vanishing
#              into BuildKit's 2 MiB step-log cap.
#   gate       FROM scratch, holds only /out. `make linux-gate` exports it
#              with `--output` and turns status.txt into the exit code.
#
# Every dependency is committed under vendor/, so the configure inside the
# image touches no network for sources and needs no token or extra build
# context. If a first-party (external/) dependency is ever declared again and
# must be fetched inside the build, use a BuildKit secret
# (--secret id=gh,env=GITHUB_TOKEN); never ARG/ENV.
#
# The build tree lives in a BuildKit cache mount, not in an image layer, so
# repeat runs are incremental and the image stays small. Reclaim the disk
# with `make linux-gate-prune` (docker builder prune -f).

ARG LLVM_MAJOR=23
ARG CMAKE_VERSION=4.4.2
ARG CMAKE_SHA256_AARCH64=9ca1aadb4451c5dcbdc67f9b4aff42dab52abbaebd8db9e2900026502dbed671
ARG CMAKE_SHA256_X86_64=3ada9a3f5d8a85413579bdd0ea6aa8e8da86efdd6d15c91a1afa517f2021956c

# A real bash 3.2 for the installer's two-shell comparison (plan 1122 M5, task 7361).
# Debian ships bash 5, so the gate cannot otherwise run the scenarios of
# scripts/install-bash32-test.sh under the shell stock macOS ships. This stage builds
# bash 3.2.57 (the last 3.2 patch level, which is what macOS's /bin/bash is) from the GNU
# source archive (ftp.gnu.org, then two mirrors if it is unreachable), verified by SHA-256 (the checksum is what makes a mirror safe), with its 2007 config.guess/config.sub replaced (they do not know aarch64) and the old K&R-era code accepted by a modern
# compiler (-std=gnu89, implicit declarations and int allowed). It builds serially (bash's
# Makefiles are not parallel-safe). Nothing else uses it.
FROM debian:trixie-slim AS bash32
ARG BASH32_VERSION=3.2.57
ARG BASH32_SHA256=3fa9daf85ebf35068f090ce51283ddeeb3c75eb5bc70b1a4a7cb05868bfe06a4
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl build-essential bison autotools-dev \
 && rm -rf /var/lib/apt/lists/*
RUN set -eu; \
    fetched=; \
    for base in https://ftp.gnu.org/gnu https://mirrors.kernel.org/gnu https://mirrors.ocf.berkeley.edu/gnu; do \
      echo "bash32: fetching ${base}/bash/bash-${BASH32_VERSION}.tar.gz"; \
      if curl -fsSL --connect-timeout 20 --max-time 300 --retry 2 -o /tmp/bash.tar.gz "${base}/bash/bash-${BASH32_VERSION}.tar.gz"; then fetched=1; echo "bash32: downloaded from ${base}"; break; fi; \
    done; \
    [ -n "${fetched}" ] || { echo "bash32: every source failed" >&2; exit 1; }; \
    echo "${BASH32_SHA256}  /tmp/bash.tar.gz" | sha256sum -c -; \
    tar -xzf /tmp/bash.tar.gz -C /tmp; \
    cd "/tmp/bash-${BASH32_VERSION}"; \
    cp /usr/share/misc/config.guess /usr/share/misc/config.sub support/; \
    CC="gcc -std=gnu89 -w" CFLAGS="-O1" \
      ./configure --prefix=/opt/bash-3.2 --without-bash-malloc --disable-nls >/tmp/configure.log 2>&1 || { tail -40 /tmp/configure.log; exit 1; }; \
    make >/tmp/make.log 2>&1 || { grep -n -B8 -A3 -E 'Error|\*\*\*' /tmp/make.log | head -80; exit 1; }; \
    make install >/tmp/install.log 2>&1 || { tail -40 /tmp/install.log; exit 1; }; \
    test "$(/opt/bash-3.2/bin/bash -c 'echo "${BASH_VERSINFO[0]}.${BASH_VERSINFO[1]}"')" = 3.2; \
    rm -rf /tmp/bash-* /tmp/bash.tar.gz

FROM debian:trixie-slim AS toolchain
ARG LLVM_MAJOR
ARG CMAKE_VERSION
ARG CMAKE_SHA256_AARCH64
ARG CMAKE_SHA256_X86_64
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl gnupg git ninja-build make pkg-config \
      python3 python3-venv sqlite3 libssl-dev zlib1g-dev procps xz-utils wget \
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

# The installer's two-shell comparison (scripts/install-bash32-test.sh) runs here with
# bash 3.2 as the old shell and the image's bash 5 as the new one, and FAILS rather than
# comparing one shell with itself when either is missing.
COPY --from=bash32 /opt/bash-3.2 /opt/bash-3.2
ENV PLANAR_BASH32_OLD=/opt/bash-3.2/bin/bash \
    PLANAR_BASH32_REQUIRE_COMPARE=1

FROM toolchain AS run
# Sibling-lane hosts are shared; the Makefile passes a bounded job count.
ARG JOBS=4
ARG CTEST_ARGS=
WORKDIR /src
COPY . /src

# Permissions regressions must run without root's filesystem bypass. Keep
# compilation privileged for compatibility with existing build cache mounts.
RUN useradd --create-home planar-test

# The whole gate is one RUN so the build cache mount is visible to every step.
# It always exits 0 and records the verdict: see the header.
RUN --mount=type=cache,target=/src/build/debug,id=planar-linux-gate-build \
    mkdir -p /out; \
    rc=0; \
    { cmake --preset debug >/out/configure.log 2>&1 || rc=$?; \
      echo "configure_exit=$rc" >>/out/status.txt; } ; \
    if [ "$rc" = 0 ]; then \
      cmake --build build/debug -j"${JOBS}" --target all planar_tests >/out/build.log 2>&1 || rc=$?; \
      echo "build_exit=$rc" >>/out/status.txt; \
    fi; \
    if [ "$rc" = 0 ]; then \
      ctest_rc=0; \
      chown -R planar-test:planar-test build/debug && \
        runuser -u planar-test -- ctest --test-dir build/debug -j"${JOBS}" --output-on-failure ${CTEST_ARGS} >/out/ctest.log 2>&1 || ctest_rc=$?; \
      echo "ctest_exit=$ctest_rc" >>/out/status.txt; \
      rc=$ctest_rc; \
    fi; \
    if [ "$rc" = 0 ]; then \
      eval_unit_rc=0; \
      PATH="/src/build/debug/bin:$PATH" make eval-orchestrator-unit >/out/eval-unit.log 2>&1 || eval_unit_rc=$?; \
      echo "eval_unit_exit=$eval_unit_rc" >>/out/status.txt; \
      rc=$eval_unit_rc; \
    fi; \
    if [ "$rc" = 0 ]; then \
      eval_queue_rc=0; \
      PATH="/src/build/debug/bin:$PATH" make eval-queue-observation-integration >/out/eval-queue-observation.log 2>&1 || eval_queue_rc=$?; \
      echo "eval_queue_observation_exit=$eval_queue_rc" >>/out/status.txt; \
      rc=$eval_queue_rc; \
    fi; \
    echo "status=$rc" >>/out/status.txt; \
    exit 0

FROM scratch AS gate
COPY --from=run /out /

# Release bundles use Bookworm's glibc 2.36 floor, independently of the
# Trixie debug gate above. Keep a separate cache for this amd64 toolchain.
FROM debian:bookworm-slim AS dist-toolchain
ARG LLVM_MAJOR
ARG LLVM_APT_URL=https://apt.llvm.org
ARG CMAKE_VERSION
ARG CMAKE_SHA256_AARCH64
ARG CMAKE_SHA256_X86_64
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl gnupg git ninja-build make pkg-config \
      python3 python3-venv sqlite3 libssl-dev zlib1g-dev procps xz-utils \
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
    echo "deb [signed-by=/usr/share/keyrings/llvm.asc] ${LLVM_APT_URL}/${VERSION_CODENAME}/ llvm-toolchain-${VERSION_CODENAME}-${LLVM_MAJOR} main" \
      > /etc/apt/sources.list.d/llvm.list; \
    echo "Installing Bookworm LLVM ${LLVM_MAJOR} from ${LLVM_APT_URL}/${VERSION_CODENAME}/"; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      clang-${LLVM_MAJOR} lld-${LLVM_MAJOR} libc++-${LLVM_MAJOR}-dev libc++abi-${LLVM_MAJOR}-dev libunwind-${LLVM_MAJOR}-dev; \
    rm -rf /var/lib/apt/lists/*

ENV PLANAR_LLVM_PREFIX=/usr/lib/llvm-${LLVM_MAJOR}

# Fail the toolchain stage, not a 20-minute configure, if the pin is unusable.
RUN set -eu; \
    cmake --version | head -1; \
    "${PLANAR_LLVM_PREFIX}/bin/clang++" --version | head -1; \
    test -d "${PLANAR_LLVM_PREFIX}/include/c++/v1"; \
    find "${PLANAR_LLVM_PREFIX}/lib" -name 'libc++.modules.json' | grep -q .; \
    for archive in libc++.a libc++abi.a libunwind.a; do \
      test -f "${PLANAR_LLVM_PREFIX}/lib/${archive}" || { echo "missing static runtime: ${archive}"; exit 1; }; \
    done

FROM dist-toolchain AS dist-run
ARG JOBS=4
# The host resolves these from the source snapshot because .git is excluded.
# scripts/dist.sh owns validation and embedding of this identity (task 7305).
ARG PLANAR_RELEASE_VERSION
ARG PLANAR_SOURCE_SHA
ARG PLANAR_SOURCE_DIRTY
ENV PLANAR_RELEASE_VERSION=${PLANAR_RELEASE_VERSION} \
    PLANAR_SOURCE_SHA=${PLANAR_SOURCE_SHA} \
    PLANAR_SOURCE_DIRTY=${PLANAR_SOURCE_DIRTY}
WORKDIR /src
COPY . /src
RUN --mount=type=cache,target=/src/build/dist,id=planar-linux-dist-bookworm-amd64 \
    case "$(uname -m)" in x86_64) ;; *) echo "unsupported Linux bundle architecture: $(uname -m)" >&2; exit 1 ;; esac && \
    make dist JOBS="${JOBS}" && \
    mkdir -p /out && cp -a /src/dist /out/dist

FROM scratch AS dist
COPY --from=dist-run /out/dist /
