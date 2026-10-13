# Spike: SMLib (NVIDIA usd-brep) as a second kernel

NVIDIA released the SMLib NURBS B-Rep kernel as
[NVIDIA-Omniverse/usd-brep](https://github.com/NVIDIA-Omniverse/usd-brep)
under Apache-2.0 on 2026-10-08. This spike answers two questions before any
oscad code changes:

1. Can the kernel be built standalone, without the Omniverse build tooling?
2. Can its output reach exact STEP, given that SMLib has no STEP writer?

Both: yes. Details below. Nothing in `src/` is touched by this spike.

## Build

```bash
git clone https://github.com/NVIDIA-Omniverse/usd-brep /path/to/usd-brep
cmake -S spike/smlib -B /tmp/smlib-build -G Ninja -DUSD_BREP_ROOT=/path/to/usd-brep
nice ninja -C /tmp/smlib-build -j10          # ~7 min: 224 kernel files, ~1M lines
cd /tmp/smlib-build && LD_LIBRARY_PATH=. ./boolean_bounds
LD_LIBRARY_PATH=. ./smlib_to_occt plate.step plate
LD_LIBRARY_PATH=. ./smlib_to_occt hard.step hard nopole
../../build/step_check plate.step hard.step
```

The CMake file compiles `source/SMLib` and `source/SM_API` directly and links
TBB. No Packman, no OpenUSD, no premake. Only the kernel and its C-style
wrapper are needed; the USD layers above them are not.

## What `smlib_to_occt` does

Every SMLib surface and curve is ultimately a NURBS (`SmPlane`, `SmCylinder`,
`SmSphere` ... derive from `SmBSplineSurface`; `SmLine`, `SmCircle` from
`SmBSplineCurve`), so the bridge pulls degree, knots, control points and
weights out uniformly and builds `Geom_BSplineSurface` / `Geom_BSplineCurve`.
Topology comes from SMLib's own shared vertices and edges (faceuse → loopuse →
edgeuse ring), vertex tolerances are set from the measured curve-end gaps,
`ShapeFix_Face` projects pcurves, and the faces are sewn into a solid.

## Results

| Fixture | SMLib | OCCT after bridge | STEP (`step_check`) |
|---|---|---|---|
| Plate 12×12×1.5 minus 16 cylinders | manifold, V=173.588 (analytic 173.588) | 22 faces, valid, V=173.588 (rel err 2.7e-6) | solids=1 valid |
| Box ∪ cylinder, fillet **every** edge r=1, minus pole-free sphere | manifold, V=4325.11 | 33 faces, valid, V=4325.11 (rel err 1.5e-6) | solids=1 valid |
| Same, with a standard (poled) sphere | manifold, V=4325.11 | 31 faces, **invalid**: the sphere face is unorientable (status 27) | reader heals it; valid |

Measured along the way:

- 16 boolean differences on the plate: 31–54 ms total (2–3 ms each).
- Max gap between a curve end and its SMLib vertex: 1.3e-7 (plate), 1.6e-6 (hard).
- Fillet of all edges of box ∪ cylinder succeeded first try.

## What is left before this could be a backend

1. **Poles.** Faces containing a surface pole (standard sphere) need their
   pcurves transferred exactly from SMLib's UV trim curves
   (`SmEdgeuse::GetUVTrimCurvePointer`) instead of projected. Or always use
   `SmApiCreateSphereNoPole`, which already passes.
2. **Analytic types.** The bridge writes every surface as a B-spline. Mapping
   `SmPlane`/`SmCylinder`/`SmCone`/`SmSphere`/`SmTorus` to their `Geom_*`
   analytic counterparts gives cleaner STEP and exact downstream recognition.
3. **Face queries for `attach()`.** Centroid, normal and planarity exist in
   SM_API; not yet exercised here.

See the GitHub issue for the backend plan.
