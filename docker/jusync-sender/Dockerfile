# syntax=docker/dockerfile:1.7
# JUSYNC streaming sender rig: builds the ANARI USD device (UsdBridge + ZMQ
# broker on :5556) against OpenUSD 25.11 and ships a VNC/noVNC desktop with
# ParaView for local end-to-end testing of the JUSYNC pipeline (the UE client
# runs on the host and connects to tcp://localhost:5556).
#
# Build context = ANARI-USD repo root:
#   docker build -f docker/jusync-sender/Dockerfile -t jusync-sender:dev .
#
# The device library itself is CPU-only (it records ANARI calls into USD), so
# a GPU is optional at build AND run; pass --gpus all to use the host GPU for
# the ANARI scivis/helium render paths or ParaView hardware GL.
#
# Base is 24.04 (noble): OpenUSD 25.11's TBB backend needs oneTBB >= 2021.6
# (jammy's 2021.5 fails tbb::task_group(ctx)); noble ships 2021.12 plus the
# cppzmq headers that jammy lacks.
ARG BASE_IMAGE=ubuntu:24.04

# --------------------------------------------------------------------------
# Stage 1: toolchain + ANARI SDK + OpenUSD + this repo's device
# --------------------------------------------------------------------------
FROM ${BASE_IMAGE} AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ninja-build git wget ca-certificates pkg-config \
      gnupg lsb-release \
      python3 python3-dev \
      libopenmpi-dev openmpi-bin \
      libzmq3-dev cppzmq-dev \
      libtbb-dev zlib1g-dev \
      libgl1-mesa-dev libglu1-mesa-dev xorg-dev libglfw3-dev mesa-common-dev \
    && rm -rf /var/lib/apt/lists/*
# CMake >= 3.28 guard: noble ships 3.28.3 (no-op there), older bases get
# Kitware's latest (also needed for ANARI-SDK main on 22.04 fallback builds).
RUN set -eux; ver=$(cmake --version | head -n1 | awk '{print $3}'); \
    if [ "$(printf '%s\n' 3.28 "$ver" | sort -V | head -n1)" != "3.28" ]; then \
      wget -qO - https://apt.kitware.com/keys/kitware-archive-latest.asc \
        | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg; \
      echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" \
        > /etc/apt/sources.list.d/kitware.list; \
      apt-get update && apt-get install -y --no-install-recommends cmake && rm -rf /var/lib/apt/lists/*; \
    fi; cmake --version | head -n1

WORKDIR /src

# --- Khronos ANARI SDK (same source the repo superbuild fetches: main) ---
ARG ANARI_SDK_REF=main
RUN git clone --depth 1 --branch ${ANARI_SDK_REF} \
      https://github.com/KhronosGroup/ANARI-SDK.git anari-sdk \
    && cmake -S anari-sdk -B build/anari -G Ninja \
         -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
         -DBUILD_EXAMPLES=OFF -DBUILD_TESTS=OFF \
         -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
         -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    && cmake --build build/anari && cmake --install build/anari

# --- OpenUSD (device targets 25.11, boost-free; core-only config) ---
ARG USD_REF=v25.11
# ninja's default job count (all cores) OOM-killed buildkitd inside the WSL2
# VM at ~45% of the USD compile once; cap concurrent g++ processes. Declared
# here (after the ANARI layer) so cached layers stay valid.
ARG MAX_JOBS=6
RUN git clone --depth 1 --branch ${USD_REF} \
      https://github.com/PixarAnimationStudios/OpenUSD.git usd \
    && cmake -S usd -B build/usd -G Ninja \
         -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/usd \
         -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
         -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
         -DPXR_BUILD_TESTS=OFF -DPXR_BUILD_EXAMPLES=OFF -DPXR_BUILD_TUTORIALS=OFF \
         -DPXR_BUILD_USDVIEW=OFF -DPXR_BUILD_IMAGING=OFF -DPXR_BUILD_USD_IMAGING=OFF \
         -DPXR_BUILD_DOCUMENTATION=OFF -DPXR_BUILD_PRMAN_PLUGIN=OFF \
         -DPXR_ENABLE_PYTHON_SUPPORT=OFF -DPXR_ENABLE_GL_SUPPORT=OFF \
         -DPXR_ENABLE_VULKAN_SUPPORT=OFF -DPXR_ENABLE_OPENVDB_SUPPORT=OFF \
         -DPXR_ENABLE_PTEX_SUPPORT=OFF -DPXR_ENABLE_MATERIALX_SUPPORT=OFF \
         -DPXR_ENABLE_HDF5_SUPPORT=OFF -DPXR_ENABLE_OCIO_SUPPORT=OFF \
         -DPXR_ENABLE_OSL_SUPPORT=OFF -DPXR_ENABLE_PROCEDURAL_SUPPORT=OFF \
         -DPXR_ENABLE_CTF_SUPPORT=OFF \
    && cmake --build build/usd -- -j ${MAX_JOBS} && cmake --install build/usd
ENV LD_LIBRARY_PATH=/opt/usd/lib

# --- ANARI USD device (this repo), with examples ---
COPY . anari-usd
RUN cmake -S anari-usd -B build/device -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr/local \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
      -DUSD_ROOT_DIR=/opt/usd -DANARI_ROOT_DIR=/usr/local \
      -DPython3_ROOT_DIR=/usr -DPython3_FIND_STRATEGY=LOCATION \
      -DUSD_DEVICE_BUILD_EXAMPLES=ON -DUSD_DEVICE_BUILD_VERSION_EXPLICIT=1 \
      -DUSD_DEVICE_USE_OMNIVERSE=OFF -DUSD_DEVICE_USE_OPENVDB=OFF \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DCMAKE_PREFIX_PATH="/opt/usd;/usr/local" \
    && cmake --build build/device -- -j ${MAX_JOBS} && cmake --install build/device

# --------------------------------------------------------------------------
# Stage 2: runtime + VNC/noVNC desktop + ParaView
# --------------------------------------------------------------------------
FROM ${BASE_IMAGE} AS runtime
ENV DEBIAN_FRONTEND=noninteractive
# Qt6 runtime for the SELF-BUILT ParaView 6.1 (bind-mounted at /opt/sender/pv,
# or baked to /opt/pv61 by Dockerfile.pvbake). apt's `paraview` used to be
# installed here and is deliberately gone: /usr/bin/paraview 5.11.2 shadowed the
# 6.1 build on PATH. libqt6svg6 supplies the SVG icon engine
# (iconengines/libqsvgicon.so) that ParaView 6 requires, otherwise every toolbar
# icon comes back a null pixmap; qt6-qpa-plugins supplies the xcb platform
# plugin for the VNC desktop.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libtbb12 zlib1g libzmq5 python3 \
      libopenmpi3 openmpi-bin \
      libgl1 libglu1-mesa mesa-utils x11-utils x11-xserver-utils \
      xterm fluxbox \
      tigervnc-standalone-server tigervnc-common \
      novnc websockify \
      less vim-tiny \
      libqt6core6t64 libqt6gui6t64 libqt6widgets6t64 libqt6network6t64 \
      libqt6opengl6t64 libqt6openglwidgets6t64 libqt6help6 libqt6core5compat6 \
      libqt6svg6 qt6-qpa-plugins libxcb-cursor0 \
    && rm -rf /var/lib/apt/lists/*

# USD device + ANARI SDK + usdcat/usdview tools live under these prefixes.
COPY --from=builder /opt/usd/  /opt/usd/
COPY --from=builder /usr/local/ /usr/local/
# ld.so must resolve the usd library's NEEDED deps (libusd_*) with no
# LD_LIBRARY_PATH: X session startup strips LD_* from the VNC desktop, so
# ParaView launched from it otherwise gets NULL from anariLoadLibrary and
# vtkAnariDevice reports "Could not load environment library" even though
# ANARI_LIBRARY is set. Also give ANARI_LIBRARY_PATH's first entry a real dir.
RUN printf '/opt/usd/lib\n' > /etc/ld.so.conf.d/zz-anari-usd.conf \
 && mkdir -p /usr/local/lib/anari \
 && ln -sf /usr/local/lib/libanari_library_usd.so /usr/local/lib/anari/ \
 && ldconfig

COPY docker/jusync-sender/entrypoint.sh   /usr/local/bin/entrypoint.sh
COPY docker/jusync-sender/run-sender.sh   /usr/local/bin/run-sender.sh
# paraview/pvserver put the self-built 6.1 on PATH (mount-first, /opt/pv61
# fallback); jusync-start.sh resolves the sender binary + snapshot dir so the
# image also starts cleanly with no build-out mount.
COPY docker/jusync-sender/paraview        /usr/local/bin/paraview
COPY docker/jusync-sender/pvserver        /usr/local/bin/pvserver
COPY docker/jusync-sender/jusync-start.sh /usr/local/bin/jusync-start.sh
RUN chmod +x /usr/local/bin/entrypoint.sh /usr/local/bin/run-sender.sh \
             /usr/local/bin/paraview /usr/local/bin/pvserver \
             /usr/local/bin/jusync-start.sh

# ANARI_LIBRARY belongs in the image, not a launcher: every ANARI consumer
# (sender, ParaView from the desktop, pvserver) needs it and none should have
# to export it. The usd library itself is found through ld.so, not
# LD_LIBRARY_PATH, because X session startup strips LD_* from the desktop.
ENV DISPLAY=:1 \
    LD_LIBRARY_PATH=/opt/usd/lib:/usr/local/lib \
    ANARI_LIBRARY=usd \
    ANARI_MODULE_PATH=/usr/local/lib/anari:/usr/local/lib \
    ANARI_LIBRARY_PATH=/usr/local/lib/anari:/usr/local/lib \
    JUSYNC_VNC_RESOLUTION=1600x900x24 \
    JUSYNC_VNC_PASSWORD=jusync

# 5556 JUSYNC broker (client), 5555 inter-rank workers, VNC 5901, noVNC 6080
EXPOSE 5556 5555 5901 6080
WORKDIR /work
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
# jusync-start.sh picks the sender binary (bind-mounted build-out first, the
# image's own /usr/local/bin/anariBigCloudUsd second) before exec'ing
# run-sender.sh, so the image starts on machines without the mount.
CMD ["/usr/local/bin/jusync-start.sh"]
