#!/bin/bash
# Starts the VNC desktop + noVNC gateway, then execs the container command.
set -e

VNC_DISP=:1
VNC_PORT=5901
NOVNC_PORT=6080

mkdir -p /root/.vnc /work /tmp/.X11-unix

# VNC password (override with JUSYNC_VNC_PASSWORD)
echo "${JUSYNC_VNC_PASSWORD:-jusync}" | vncpasswd -f > /root/.vnc/passwd
chmod 600 /root/.vnc/passwd

cat > /root/.vnc/config <<EOF
geometry=${JUSYNC_VNC_RESOLUTION:-1600x900x24}
depth=24
localhost=no
securitytypes=vncauth
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

echo "[entrypoint] desktop ${VNC_DISP} -> VNC ${VNC_PORT}, noVNC http://localhost:${NOVNC_PORT}/vnc.html (password: ${JUSYNC_VNC_PASSWORD:-jusync})"

# Optional keep-up mode: never let the container die with the command.
if [ "${JUSYNC_KEEPUP:-0}" = "1" ]; then
    "$@" || true
    echo "[entrypoint] command exited; JUSYNC_KEEPUP=1 -> staying up (docker exec -it <c> bash)"
    tail -f /dev/null
else
    exec "$@"
fi
