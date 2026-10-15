#!/usr/bin/env python3
"""Generate a big, dramatically time-varying point cloud as a ParaView .pvd series.

Output mirrors the structure of the images' own /opt/sender/anim/steps/*.vtu:
VTK XML UnstructuredGrid, N points, N vertex cells, inline base64 binary arrays
(`format="binary"`, header_type="UInt32") -- the layout this rig is proven to load
and stream to UE.

The animation is deliberately huge: it starts as a tiny dense core and expands
into a wide, flattening, winding spiral, rotating ~0.85 rad every step, with
turbulence and a full hue change per step. Consecutive samples look nothing
alike, which is the point (easy to confirm in UE which sample is on screen).

    python3 make_bigcloud_timesteps.py [outdir]

Env: BIGCLOUD_POINTS (10000000), BIGCLOUD_STEPS (8), BIGCLOUD_DT (1.0),
     BIGCLOUD_ARMS (5), BIGCLOUD_NAME (bigcloud10m)
"""

import base64
import math
import os
import struct
import sys

import numpy as np

N = int(os.environ.get("BIGCLOUD_POINTS", "10000000"))
STEPS = int(os.environ.get("BIGCLOUD_STEPS", "8"))
DT = float(os.environ.get("BIGCLOUD_DT", "1.0"))
ARMS = int(os.environ.get("BIGCLOUD_ARMS", "5"))
NAME = os.environ.get("BIGCLOUD_NAME", "bigcloud10m")

VTK_VERTEX = 1


def b64(raw: bytes) -> bytes:
    """Inline binary block: UInt32 byte-count header + payload, base64."""
    return base64.b64encode(struct.pack("<I", len(raw)) + raw)


def write_vtu(path, pos, colours):
    n = pos.shape[0]
    conn = np.arange(n, dtype="<i4")
    offs = np.arange(1, n + 1, dtype="<i4")
    types = np.full(n, VTK_VERTEX, dtype=np.uint8)

    with open(path, "wb") as f:
        f.write(b'<?xml version="1.0"?>\n')
        f.write(b'<VTKFile type="UnstructuredGrid" version="0.1" '
                b'byte_order="LittleEndian" header_type="UInt32">\n')
        f.write(('<UnstructuredGrid NumberOfPoints="%d" NumberOfCells="%d">\n'
                 % (n, n)).encode())
        f.write(b'<PointData>\n')
        f.write(b'<DataArray type="UInt8" Name="Colors" NumberOfComponents="4" format="binary">\n')
        f.write(b64(colours.tobytes()))
        f.write(b'\n</DataArray>\n</PointData>\n<CellData/>\n')
        f.write(b'<Points>\n')
        f.write(b'<DataArray type="Float32" Name="Points" NumberOfComponents="3" format="binary">\n')
        f.write(b64(pos.tobytes()))
        f.write(b'\n</DataArray>\n</Points>\n')
        f.write(b'<Cells>\n')
        f.write(b'<DataArray type="Int32" Name="connectivity" format="binary">\n')
        f.write(b64(conn.tobytes()))
        f.write(b'\n</DataArray>\n')
        f.write(b'<DataArray type="Int32" Name="offsets" format="binary">\n')
        f.write(b64(offs.tobytes()))
        f.write(b'\n</DataArray>\n')
        f.write(b'<DataArray type="UInt8" Name="types" format="binary">\n')
        f.write(b64(types.tobytes()))
        f.write(b'\n</DataArray>\n</Cells>\n')
        f.write(b'</UnstructuredGrid>\n</VTKFile>\n')


def write_pvd(path, entries):
    lines = ['<?xml version="1.0"?>',
             '<VTKFile type="Collection" version="0.1">',
             '  <Collection>']
    for t, f in entries:
        lines.append('    <DataSet timestep="%g" group="" part="0" file="%s"/>' % (t, f))
    lines += ['  </Collection>', '</VTKFile>', '']
    with open(path, "w") as fh:
        fh.write("\n".join(lines))


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(outdir, exist_ok=True)

    rng = np.random.default_rng(20261013)
    # Fixed per-point identity: radius fraction, arm, phase, height, turbulence dir.
    u = rng.random(N).astype(np.float32)
    arm = rng.integers(0, ARMS, N).astype(np.float32)
    th0 = (rng.random(N).astype(np.float32) * (2.0 * math.pi))
    z0 = rng.normal(0.0, 1.0, N).astype(np.float32)
    noise = rng.normal(0.0, 1.0, (N, 3)).astype(np.float32)
    arm_off = arm * (2.0 * math.pi / ARMS)

    entries = []
    for s in range(STEPS):
        phase = s / max(1, STEPS - 1)

        radius = 4.0 + 196.0 * (phase ** 1.6)      # tight core -> huge cloud
        flatten = 1.0 - 0.90 * phase               # sphere -> flat disc
        swirl = 14.0 * phase                       # no winding -> tight spiral
        turb = 0.02 + 0.50 * phase
        rot = s * 0.85                             # obvious per-step rotation
        prof = 1.8 - 1.2 * phase                   # dense core -> even disc
        # one shell/ring step so the shape family changes, not just the scale
        if s == (STEPS // 2):
            prof = -0.55

        r_frac = 0.20 + 0.80 * u
        r = radius * np.power(r_frac, prof).astype(np.float32)
        th = th0 + arm_off + swirl * r_frac + rot
        z = (z0 * flatten * radius * 0.35).astype(np.float32)

        pos = np.empty((N, 3), dtype=np.float32)
        pos[:, 0] = r * np.cos(th)
        pos[:, 1] = r * np.sin(th)
        pos[:, 2] = z
        pos += (turb * radius) * noise

        # full hue change per step + radial gradient, so every sample is obvious
        hue = (0.06 + s * 0.37) % 1.0
        rg = np.clip(r_frac, 0.0, 1.0)
        rr = np.clip(np.abs(((hue + 0.00) % 1.0) * 6.0 - 3.0) - 1.0, 0, 1) * rg
        gg = np.clip(np.abs(((hue + 0.33) % 1.0) * 6.0 - 3.0) - 1.0, 0, 1) * rg
        bb = np.clip(np.abs(((hue + 0.66) % 1.0) * 6.0 - 3.0) - 1.0, 0, 1) * rg
        colours = np.empty((N, 4), dtype=np.uint8)
        colours[:, 0] = (rr * 235.0 + 20.0).astype(np.uint8)
        colours[:, 1] = (gg * 235.0 + 20.0).astype(np.uint8)
        colours[:, 2] = (bb * 235.0 + 20.0).astype(np.uint8)
        colours[:, 3] = 255

        fname = "%s_%05d.vtu" % (NAME, s)
        write_vtu(os.path.join(outdir, fname), pos, colours)
        entries.append((s * DT, fname))
        print("  step %d: R=%.1f flat=%.2f swirl=%.1f turb=%.2f hue=%.2f -> %s (%.0f MB)"
              % (s, radius, flatten, swirl, turb, hue, fname,
                 os.path.getsize(os.path.join(outdir, fname)) / 1e6))

    write_pvd(os.path.join(outdir, "%s.pvd" % NAME), entries)
    total = sum(os.path.getsize(os.path.join(outdir, f)) for _, f in entries)
    print("wrote %s.pvd : %d steps, %d points/step, %.1f GB total"
          % (NAME, STEPS, N, total / 1e9))


if __name__ == "__main__":
    main()
