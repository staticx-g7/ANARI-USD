#!/bin/sh
# Container start command: resolve the sender binary and RawGeo snapshot dir
# before handing off to run-sender.sh.
#
# Without this, JUSYNC_SENDER_BIN=/opt/sender/anariBigCloudUsd is baked into
# the image env and a machine with no build-out bind mount fails at start even
# though the image ships its own /usr/local/bin/anariBigCloudUsd.
BIN=/usr/local/bin/anariBigCloudUsd
[ -x /opt/sender/anariBigCloudUsd ] && BIN=/opt/sender/anariBigCloudUsd
export JUSYNC_SENDER_BIN="$BIN"

# Same story for snapshots: /opt/sender/... only makes sense when the mount is
# actually there, otherwise writes land invisibly on the container layer.
case "${JUSYNC_RAWGEO_SNAPSHOT_DIR:-}" in
    /opt/sender/*)
        if [ ! -d /opt/sender/pv ]; then
            export JUSYNC_RAWGEO_SNAPSHOT_DIR=/work/snaps
        fi
        ;;
esac

echo "[jusync-start] sender=$BIN snapshots=${JUSYNC_RAWGEO_SNAPSHOT_DIR:-unset}"
exec /usr/local/bin/run-sender.sh "$BIN"
