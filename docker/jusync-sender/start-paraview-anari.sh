#!/bin/sh
# Launch ParaView 6.1 rendering through the ANARI 'usd' device library.
# Run inside the jusync-sender container. Stop the bigCloud streamer FIRST
# (docker stop is not needed; kill the sender app) so port 5556 is free:
#   docker exec jusync-sender pkill -f anariBigCloudUsd
set -e
export DISPLAY=:1
export LD_LIBRARY_PATH=/tmp/anarilib:/opt/sender/pv/lib:/usr/local/lib:${LD_LIBRARY_PATH}
# ANARI: make VTK's "environment" library selection resolve to our usd device
export ANARI_LIBRARY=usd
# Load from container-native storage: the Windows bind-mount file gets stale
# mmap pages in Docker Desktop's 9p cache whenever it is hot-replaced, which
# segfaults ld.so (elf_dynamic_do_Rela) even though read() shows correct bytes.
# restart-paraview copies build-out -> /tmp/anarilib (native overlay) instead.
export ANARI_LIBRARY_PATH=/tmp/anarilib
# Software GL is fine for Storm (llvmpipe); force it for deterministic Xlib/GLX
export LIBGL_ALWAYS_SOFTWARE=1
# JUSYNC time-sample streaming: the device detects ParaView's TemporalSnapshot
# timeline inside commits and streams each timestep through the RawGeo path.
# idle-suppression off so looping animation is never mistaken for a quiet scene.
export JUSYNC_RAWGEO=1
# Interactive sessions stream every point; PV_STRIDE=8 paraview thins them.
# JUSYNC_RAWGEO_STRIDE is deliberately NOT the default source: the sender base
# image bakes it to 4 for 10M-point clouds and a baked ENV always beats
# ${VAR:-1}, which silently turned a 10560-point dataset into 2640. The sender
# that needs decimation sets it in run-sender.sh instead.
export JUSYNC_RAWGEO_STRIDE=${PV_STRIDE:-1}
export JUSYNC_RAWGEO_IDLE_FRAMES=0
export JUSYNC_ANIM_FPS=4
export JUSYNC_ANIM_DT=0.25
export JUSYNC_ANIM_MAXSAMPLES=24
# Cap how often a committed frame is actually stored+notified. Playing the
# animation loops VTK commits as fast as it can; without this the render/GUI
# thread saturates. 0 (or unset) = unlimited.
export JUSYNC_ANIM_PUBLISH_HZ=3
PVDIR=${PVDIR:-/opt/sender/pv}
# Auto-driver: when present, the script builds the pipeline, scrubs all 16
# timesteps (visibility-forced commits) and keeps rendering for the ZMQ pump.
SCRIPT_ARG=""
if [ -f /tmp/jusync_capture.py ]; then SCRIPT_ARG="--script /tmp/jusync_capture.py"; fi
exec "$PVDIR/bin/paraview" $SCRIPT_ARG "$@"
