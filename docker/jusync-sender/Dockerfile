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

ARG BASE_IMAGE=ubuntu:22.04

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
      libzmq3-dev \
      libtbb-dev zlib1g-dev \
      libgl1-mesa-dev libglu1-mesa-dev xorg-dev libglfw3-dev mesa-common-dev \
    && rm -rf /var/lib/apt/lists/*
# Ubuntu 22.04 ships CMake 3.22; ANARI-SDK main / OpenUSD 25.x want >= 3.28.
RUN set -eux; ver=$(cmake --version | head -n1 | awk '{print $3}'); \
    if [ "$(printf '%s\n' 3.28 "$ver" | sort -V | head -n1)" != "3.28" ]; then \
      wget -qO - https://apt.kitware.com/keys/kitware-archive-latest.asc \
        | gpg --dearmor -o /usr/share/keyrings/kitware-archive-keyring.gpg; \
      echo "deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main" \
        > /etc/apt/sources.list.d/kitware.list; \
      apt-get update && apt-get install -y --no-install-recommends cmake && rm -rf /var/lib/apt/lists/*; \
    fi; cmake --version | head -n1

WORKDIR /src

# --- cppzmq (C++ zmq.hpp headers; Ubuntu 22.04 has no cppzmq package) ------
# Header-only: zmq's C lib comes from libzmq3-dev above. The device's ZmqBroker
# needs zmq.hpp (find_package(cppzmq) / find_path(zmq.hpp) fallback).
ARG CPPZMQ_REF=v4.11.0
RUN git clone --depth 1 --branch ${CPPZMQ_REF} https://github.com/zeromq/cppzmq.git cppzmq \
    && cmake -S cppzmq -B build/cppzmq -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_INSTALL_PREFIX=/usr/local -DCPPZMQ_BUILD_TESTS=OFF \
    && cmake --build build/cppzmq --target install

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
    && cmake --build build/usd && cmake --install build/usd
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
    && cmake --build build/device && cmake --install build/device

# --------------------------------------------------------------------------
# Stage 2: runtime + VNC/noVNC desktop + ParaView
# --------------------------------------------------------------------------
FROM ${BASE_IMAGE} AS runtime
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates libtbb2 zlib1g libzmq5 python3 \
      libopenmpi3 openmpi-bin \
      libgl1 libglu1-mesa mesa-utils x11-utils x11-xserver-utils \
      xterm fluxbox \
      tigervnc-standalone-server tigervnc-common \
      novnc websockify \
      paraview \
      less vim-tiny \
    && rm -rf /var/lib/apt/lists/*

# USD device + ANARI SDK + usdcat/usdview tools live under these prefixes.
COPY --from=builder /opt/usd/  /opt/usd/
COPY --from=builder /usr/local/ /usr/local/
RUN ldconfig

COPY docker/jusync-sender/entrypoint.sh   /usr/local/bin/entrypoint.sh
COPY docker/jusync-sender/run-sender.sh   /usr/local/bin/run-sender.sh
RUN chmod +x /usr/local/bin/entrypoint.sh /usr/local/bin/run-sender.sh

ENV DISPLAY=:1 \
    LD_LIBRARY_PATH=/opt/usd/lib:/usr/local/lib \
    ANARI_MODULE_PATH=/usr/local/lib/anari:/usr/local/lib \
    ANARI_LIBRARY_PATH=/usr/local/lib/anari:/usr/local/lib \
    JUSYNC_VNC_RESOLUTION=1600x900x24 \
    JUSYNC_VNC_PASSWORD=jusync

# 5556 JUSYNC broker (client), 5555 inter-rank workers, VNC 5901, noVNC 6080
EXPOSE 5556 5555 5901 6080
WORKDIR /work
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["/usr/local/bin/run-sender.sh"]
