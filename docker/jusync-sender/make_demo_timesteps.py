#!/usr/bin/env python3
"""Generate a ParaView-loadable time series for the JUSYNC rig.

Produces two independent .pvd collections in the output directory:

  points.pvd   Rotating spiral point cloud (PolyData, explicit verts).
               Load it and use the *Points* representation -> the ANARI mapper
               emits a sphere/instancer geometry, which the JUSYNC device mirrors
               to clips/<name>_Geom__<t>.geo (the RawGeo point-cloud path).

  mesh.pvd     Ripple-deformed grid surface (PolyData, triangles + normals).
               Use the *Surface* representation -> the normal USD mesh clip path.

Both share the same time steps, so loading both gives one scene containing a
time-varying mesh AND a time-varying point cloud.

    python3 make_demo_timesteps.py [outdir]

Env overrides: DEMO_STEPS, DEMO_DT, DEMO_POINTS, DEMO_MESH_DIM.
"""

import math
import os
import sys

STEPS = int(os.environ.get("DEMO_STEPS", "12"))
DT = float(os.environ.get("DEMO_DT", "0.5"))
NPOINTS = int(os.environ.get("DEMO_POINTS", "12000"))
DIM = int(os.environ.get("DEMO_MESH_DIM", "64"))


def fnum(v):
    return "%.6g" % v


def write_pvd(path, entries):
    lines = ['<?xml version="1.0"?>',
             '<VTKFile type="Collection" version="0.1">',
             '  <Collection>']
    for t, fname in entries:
        lines.append('    <DataSet timestep="%s" group="" part="0" file="%s"/>' % (fnum(t), fname))
    lines += ['  </Collection>', '</VTKFile>', '']
    with open(path, "w", encoding="ascii") as fh:
        fh.write("\n".join(lines))


def write_vtp(path, points, colors, polys, verts=False):
    """colors: list of (r,g,b,a) 0-255. polys: list of index tuples. verts: one
    cell per point (a loadable point cloud), mutually exclusive with polys."""
    npts = len(points)
    npolys = len(polys)
    nverts = npts if (verts and not npolys) else 0
    out = []
    out.append('<?xml version="1.0"?>')
    out.append('<VTKFile type="PolyData" version="1.0" byte_order="LittleEndian">')
    out.append('  <PolyData>')
    out.append('    <Piece NumberOfPoints="%d" NumberOfVerts="%d" NumberOfLines="0" '
               'NumberOfStrips="0" NumberOfPolys="%d">' % (npts, nverts, npolys))

    out.append('      <PointData Scalars="Colors">')
    out.append('        <DataArray type="UInt8" Name="Colors" NumberOfComponents="4" format="ascii">')
    row = []
    for c in colors:
        row.append("%d %d %d %d" % c)
        if len(row) == 4:
            out.append("          " + " ".join(row))
            row = []
    if row:
        out.append("          " + " ".join(row))
    out.append('        </DataArray>')
    out.append('      </PointData>')
    out.append('      <CellData/>')

    out.append('      <Points>')
    out.append('        <DataArray type="Float32" Name="Points" NumberOfComponents="3" format="ascii">')
    row = []
    for p in points:
        row.append("%s %s %s" % (fnum(p[0]), fnum(p[1]), fnum(p[2])))
        if len(row) == 3:
            out.append("          " + " ".join(row))
            row = []
    if row:
        out.append("          " + " ".join(row))
    out.append('        </DataArray>')
    out.append('      </Points>')

    # Verts carry the point cloud; Polys carry the mesh. Either may be empty.
    out.append('      <Verts>')
    out.append('        <DataArray type="Int64" Name="connectivity" format="ascii">')
    if nverts > 0:
        line = []
        for i in range(npts):
            line.append(str(i))
            if len(line) == 12:
                out.append("          " + " ".join(line))
                line = []
        if line:
            out.append("          " + " ".join(line))
    out.append('        </DataArray>')
    out.append('        <DataArray type="Int64" Name="offsets" format="ascii">')
    if nverts > 0:
        line = []
        for i in range(npts):
            line.append(str(i + 1))
            if len(line) == 12:
                out.append("          " + " ".join(line))
                line = []
        if line:
            out.append("          " + " ".join(line))
    out.append('        </DataArray>')
    out.append('      </Verts>')

    out.append('      <Polys>')
    out.append('        <DataArray type="Int64" Name="connectivity" format="ascii">')
    if npolys > 0:
        flat = []
        for tri in polys:
            flat.extend(str(i) for i in tri)
        line = []
        for tok in flat:
            line.append(tok)
            if len(line) == 12:
                out.append("          " + " ".join(line))
                line = []
        if line:
            out.append("          " + " ".join(line))
    out.append('        </DataArray>')
    out.append('        <DataArray type="Int64" Name="offsets" format="ascii">')
    if npolys > 0:
        line = []
        for k in range(1, npolys + 1):
            line.append(str(k * 3))
            if len(line) == 12:
                out.append("          " + " ".join(line))
                line = []
        if line:
            out.append("          " + " ".join(line))
    out.append('        </DataArray>')
    out.append('      </Polys>')

    out.append('    </Piece>')
    out.append('  </PolyData>')
    out.append('</VTKFile>')
    out.append('')
    with open(path, "w", encoding="ascii") as fh:
        fh.write("\n".join(out))


def cloud_points(step):
    """Spiral shell rotating about Z; radius pulse makes motion unmistakable."""
    ang0 = step * 0.45
    pulse = 1.0 + 0.10 * math.sin(step * 0.7)
    out = []
    colors = []
    for i in range(NPOINTS):
        u = (i + 0.5) / NPOINTS
        arm = i % 5
        r = 8.0 + 26.0 * math.sqrt(u)
        th = ang0 + u * 9.0 + (arm * 2.0 * math.pi / 5.0)
        hi = 12.0 * math.sin(u * math.pi * 3.0 + step * 0.3)
        r *= pulse
        x = r * math.cos(th)
        y = r * math.sin(th)
        z = hi
        out.append((x, y, z))
        t = u
        colors.append((int(40 + 200 * t), int(90 + 120 * (1 - t)), int(230 - 150 * t), 255))
    return out, colors


def mesh_points(step):
    """Grid surface with a travelling ripple; returns points, colours, tris."""
    extent = 55.0
    dim = DIM
    pts = []
    cols = []
    base = []
    for j in range(dim):
        for i in range(dim):
            x = (-1.0 + 2.0 * i / (dim - 1)) * extent
            y = (-1.0 + 2.0 * j / (dim - 1)) * extent
            r = math.hypot(x, y) + 1e-4
            fall = math.exp(-(r * r) * (0.6 / (extent * extent)))
            amp = 0.22 * extent * fall
            arg = r * 0.05 - step * 0.55
            z = amp * math.sin(arg)
            pts.append((x, y + 110.0, z))
            base.append(z)
    lo, hi = min(base), max(base)
    span = (hi - lo) or 1.0
    for z in base:
        h = (z - lo) / span
        cols.append((int(60 + 180 * h), int(70 + 70 * (1 - h)), int(240 - 160 * h), 255))

    tris = []
    for j in range(dim - 1):
        for i in range(dim - 1):
            v00 = j * dim + i
            v10 = j * dim + i + 1
            v01 = (j + 1) * dim + i
            v11 = (j + 1) * dim + i + 1
            tris.append((v00, v10, v11))
            tris.append((v00, v11, v01))
    return pts, cols, tris


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(outdir, exist_ok=True)

    p_entries = []
    m_entries = []
    for step in range(STEPS):
        t = step * DT

        pts, cols = cloud_points(step)
        pf = "points_%05d.vtp" % step
        write_vtp(os.path.join(outdir, pf), pts, cols, [], verts=True)
        p_entries.append((t, pf))

        mpts, mcols, mtris = mesh_points(step)
        mf = "mesh_%05d.vtp" % step
        write_vtp(os.path.join(outdir, mf), mpts, mcols, mtris)
        m_entries.append((t, mf))

    write_pvd(os.path.join(outdir, "points.pvd"), p_entries)
    write_pvd(os.path.join(outdir, "mesh.pvd"), m_entries)

    print("wrote %d steps to %s" % (STEPS, os.path.abspath(outdir)))
    print("  points.pvd  -> load + Points representation (raw point path)")
    print("  mesh.pvd    -> load + Surface representation (mesh path)")


if __name__ == "__main__":
    main()
