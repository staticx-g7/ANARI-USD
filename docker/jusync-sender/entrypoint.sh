#!/bin/bash
# Starts the VNC desktop + noVNC gateway, then execs the container command.
set -e

VNC_DISP=:1
VNC_PORT=5901
NOVNC_PORT=6080

mkdir -p /root/.vnc /work /tmp/.X11-unix

# Clear stale X11 runtime state. An image committed from a RUNNING container
# bakes /tmp/.X1-lock and /tmp/.X11-unix/X1 into it, and tigervnc then refuses
# to start with ":1 is taken because of /tmp/.X1-lock" on every fresh container.
rm -f /tmp/.X*-lock /tmp/.X11-unix/X*

# VNC password (override with JUSYNC_VNC_PASSWORD).
# NOTE: Ubuntu 24.04's tigervnc packages no longer ship vncpasswd; fall back
# to unauthenticated VNC on this throwaway Xvnc. run.ps1 / docker-compose bind
# the mapped ports to 127.0.0.1, so only this machine can reach the desktop.
VNC_SEC=vncauth
if command -v vncpasswd >/dev/null 2>&1; then
    echo "${JUSYNC_VNC_PASSWORD:-jusync}" | vncpasswd -f > /root/.vnc/passwd
    chmod 600 /root/.vnc/passwd
else
    echo "[entrypoint] vncpasswd unavailable (tigervnc on noble) - VNC auth off, relying on 127.0.0.1 port binding" >&2
    VNC_SEC=none
fi

cat > /root/.vnc/config <<EOF
geometry=${JUSYNC_VNC_RESOLUTION:-1600x900x24}
depth=24
localhost=no
securitytypes=${VNC_SEC}
EOF

# Xstartup: light WM + terminal so ParaView windows behave
cat > /root/.vnc/Xstartup <<'EOF'
#!/bin/sh
fluxbox &
xterm -geometry 100x30+10+10 -title jusync-console &
EOF
chmod +x /root/.vnc/Xstartup

vncserver ${VNC_DISP} -rfbport ${VNC_PORT} -localhost no \
    ${JUSYNC_VNC_RESOLUTION:+-geometry ${JUSYNC_VNC_RESOLUTION}} || {
        echo "[entrypoint] VNC failed to start; continuing headless" >&2
    }

DISPLAY=${VNC_DISP} xsetroot -cursor_name left_ptr 2>/dev/null || true

# noVNC browser gateway (http://localhost:6080/vnc.html)
if [ "${JUSYNC_NOVNC:-1}" = "1" ]; then
    websockify --web=/usr/share/novnc/ ${NOVNC_PORT} localhost:${VNC_PORT} >/tmp/websockify.log 2>&1 &
fi

# Only claim the desktop is up when the X socket actually exists. This echo ran
# unconditionally before, so a failed VNC start still logged "desktop :1 -> VNC
# 5901" and a headless container looked healthy.
if [ ! -e "/tmp/.X11-unix/X${VNC_DISP#:}" ]; then
    echo "[entrypoint] WARNING: no X socket for ${VNC_DISP} - desktop unavailable" >&2
elif [ "$VNC_SEC" = vncauth ]; then
    echo "[entrypoint] desktop ${VNC_DISP} -> VNC ${VNC_PORT}, noVNC http://localhost:${NOVNC_PORT}/vnc.html (password: ${JUSYNC_VNC_PASSWORD:-jusync})"
else
    echo "[entrypoint] desktop ${VNC_DISP} -> VNC ${VNC_PORT} (no auth), noVNC http://localhost:${NOVNC_PORT}/vnc.html"
fi

# Optional keep-up mode: never let the container die with the command.
if [ "${JUSYNC_KEEPUP:-0}" = "1" ]; then
    "$@" || true
    echo "[entrypoint] command exited; JUSYNC_KEEPUP=1 -> staying up (docker exec -it <c> bash)"
    tail -f /dev/null
else
    exec "$@"
fi
