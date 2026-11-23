# syntax=docker/dockerfile:1.7
# DesertServer for Linux x86-64: the dedicated server image (Docs/contracts/NET-ARCH.md §6, §9 row NET-MESH-0).
# UE ships a dedicated server as the same modules cross-built for Linux and run as one container per role
# (Agones-style fleets); this is that image for the mesh of one (`--role all`).
#
# Two stages. `build`: Ubuntu + clang + CMake + Ninja configure the preset linux-server-clang (DESERT_SERVER_ONLY:
# the module table, the vendored libraries, the header tool and DesertServer; no Vulkan SDK, no window system, no
# editor or player) and build the one target. `runtime`: the binary alone on a bare Ubuntu; the cooked world and its
# project are MOUNTED, never baked in, so one image serves every world:
#
#   docker build --platform linux/amd64 -f Server/Dockerfile -t desert-server .
#   docker run --platform linux/amd64 -v <project dir>:/project:ro -v <world dir>:/world desert-server \
#       --project /project/Desert.deproj --role all --world /world/WP_Protocol.desce --source 0,0,0 --steps 600
#
# The context is the repository root WITH its submodules checked out (CI: actions/checkout submodules: recursive).
# A working tree whose submodules are symlinks into another tree (scripts/Dev/new_tree.sh) has to be staged with
# the links dereferenced first: Docker copies a symlink as a symlink. Server/.dockerignore keeps the context to the
# sources the server build reads.
# The CI job server-linux (.github/workflows/ci.yml) builds this same file.

FROM --platform=linux/amd64 ubuntu:24.04 AS build
ARG DEBIAN_FRONTEND=noninteractive
# clang 18 (Ubuntu 24.04's default), CMake 3.28 (the project minimum), Ninja; OpenSSL's libcrypto for
# GameNetworkingSockets (cmake/ThirdParty/GameNetworkingSockets.cmake, linked statically); ccache for rebuilds.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        clang lld cmake ninja-build ccache git ca-certificates libssl-dev file \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
ENV CCACHE_DIR=/ccache
RUN --mount=type=cache,target=/ccache \
    cmake --preset linux-server-clang -DDESERT_COMPILER_LAUNCHER=ccache \
 && cmake --build --preset linux-server-clang-release \
 && file build/Bin/Release/DesertServer

FROM --platform=linux/amd64 ubuntu:24.04 AS runtime
# The binary needs libstdc++/libc only (OpenSSL's libcrypto is linked statically); `file` stays for the image's
# own self-description (the scenario's ELF check).
RUN apt-get update \
 && apt-get install -y --no-install-recommends file \
 && rm -rf /var/lib/apt/lists/* \
 && useradd --system --uid 10001 desert
COPY --from=build /src/build/Bin/Release/DesertServer /opt/desert/DesertServer
USER desert
WORKDIR /opt/desert
ENTRYPOINT ["/opt/desert/DesertServer"]
