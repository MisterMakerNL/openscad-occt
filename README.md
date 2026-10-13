# oscad — OpenSCAD on the OpenCASCADE (OCCT) kernel

A clean-room, minimal [OpenSCAD](https://openscad.org)-style interpreter that uses
**OpenCASCADE Technology (OCCT)** as its CAD kernel instead of a mesh kernel
(CGAL/Manifold). Because OCCT is a **B-Rep** (boundary-representation) kernel,
geometry stays exact end-to-end: primitives are analytic surfaces, booleans are
exact, and models export to **STEP** as real solids — not tessellated meshes.

Why OCCT:
- **STEP/IGES export** — exact solids for real CAD interoperability.
- **Boolean robustness** — exact B-Rep booleans, no mesh coplanarity / non-manifold artifacts.

> **See it in action:** **[examples/](examples/)** — a bracket, flange, pulley and
> hex standoff, each an exact STEP solid (cones, tori, cylinders — not triangles), two
> with declarative fillets OpenSCAD can't express.
>
> **Design rationale, conceptual findings, and an honest viability read:**
> see **[DESIGN.md](DESIGN.md)** — the "why" behind this (the empty *declarative ×
> B-Rep* quadrant, why fillet isn't pure CSG, selection-as-query, where the ceiling is).

## Status — core CSG + 2D + B-Rep features + a real language ✅

| Area        | Supported |
|-------------|-----------|
| 3D primitives | `cube`, `sphere`, `cylinder` (incl. cones via `r1`/`r2`) |
| 2D primitives | `square`, `circle` (`$fn`≥3 → exact n-gon), `polygon` |
| 2D → 3D     | `linear_extrude(height, center)`, `rotate_extrude(angle)` |
| Transforms  | `translate`, `rotate` (Euler + axis-angle), `scale`, `mirror` |
| Booleans    | `union`, `difference`, `intersection` — 2D (faces-with-holes) and 3D |
| Features    | `fillet(r, edges="all"\|"convex"\|"concave")` / `round(...)` — exact B-Rep edge rounding (beyond OpenSCAD) |
| Assembly    | `attach(on=…[, from=…]) { parent; child… }` — seat children onto a queried face by exact datum frames (beyond OpenSCAD) |
| Language    | numbers, vectors, strings, ranges; `+ - * / %`, comparison, `&& \|\|`, ternary `?:`, indexing `[]`; `name = expr;`, named/positional args, `$fn` |
| Control flow | user `module`/`function` (incl. recursion + `children()`), `for`, `if`/`else`, built-in math (`sin`…`sqrt`, `pow`, `min`, `max`, `len`, `norm`, `cross`) |
| Output      | binary **STL** (tessellated) and **STEP** (exact B-Rep) |

`fillet` is the *declarative feature* layer: which edges to round is a **pure query
over topology** (`edges="convex"`), evaluated against the freshly-built solid, not a
stateful interactive pick — which makes it *more* parametrically robust than a GUI
feature tree (the predicate re-resolves every rebuild). `fillet(2) cube(20)` stays
exactly 20mm (in-place, unlike a grow-the-part Minkowski) and exports as STEP with
exact `CYLINDRICAL_SURFACE` edge blends + `SPHERICAL_SURFACE` corner patches.
Convex/concave classification uses OCCT's own `BRepOffset_Analyse` (the classifier
its fillet/offset code relies on). On a step solid, `"convex"` and `"concave"`
exactly partition the filletable edges. Fillet is *partial* — too large an `r` for
some face fails, and we surface that rather than emit a wrong shape. Next rungs:
datum-relative selection, then arbitrary predicates.

`attach` is the *declarative assembly* layer — the **constructive dual** of `fillet`'s
selection. Where selection asks "which sub-topology do I *operate on*?", a datum asks
"which sub-topology do I *measure a frame from, and mate to*?" `attach(on="top") { plate;
boss; }` seats the boss's face onto the plate's top face by coinciding two frames derived
*exactly* from the B-Rep faces (centroid + outward normal), with **no global coordinate
arithmetic** — change the plate thickness and the boss follows the top face. This is
OpenSCAD's most-felt gap (no local origin for a subassembly): there you'd hand-replay the
parent's transforms at the call site; here positioning is a pure query over topology,
re-resolved each rebuild. Faces are named by outward-normal word
(`top|bottom|left|right|front|back`); `from` defaults to the opposite of `on`. Like
fillet, `attach` is *partial* and **fails loud** — a cylinder has no planar face to seat
flat against a wall, so it says so rather than faking a mate.

Validated: outputs are valid single solids (`BRepCheck_Analyzer` = valid, round-trip
through OCCT's STEP reader), bounding boxes match reference OpenSCAD, and curved
surfaces are carried *exactly* (e.g. a sphere bite exports as a `SPHERICAL_SURFACE`,
a tapered cylinder as a `CONICAL_SURFACE`).

2D shapes become planar faces in z=0; 2D booleans yield faces-with-holes that
`linear_extrude` turns into solids with holes; `rotate_extrude` maps the profile's
`(x,y)` to `(radius, height)` and revolves about Z (a torus exports as an exact
`TOROIDAL_SURFACE`). Unsupported extrude options (`twist=`, `scale=`, `start=`)
**fail loud** rather than emit a silently-wrong shape.

### Not yet (future milestones)
list comprehensions (`[for …]`, `let`, `each`), `echo`/`assert`, `twist`/`scale`
`linear_extrude` and `start` `rotate_extrude`, `hull`/`minkowski`/`offset`, `import`,
text, and a GL preview GUI.

## Architecture

```
.scad source
  → Lexer        src/Lexer.*      tokens
  → Parser       src/Parser.*     AST            (src/Ast.h)
  → Evaluator    src/Evaluator.*  CSG node tree  (src/Node.h, values in src/Value.h)
  → Backend      src/Backend.h    the kernel seam: build(tree) → opaque Solid → writeStl/writeStep
      occt       src/occt/*       OCCT TopoDS_Shape; BRepMesh→STL, STEPControl_Writer→STEP  (default)
      smlib      src/smlib/*      SMLib SmBrep; own tessellator→STL, SmBrep→OCCT bridge→STEP  (optional)
```

The front-end (lexer/parser/evaluator) is kernel-agnostic: it produces a pure-data
CSG node tree. A `Backend` turns the tree into geometry; nothing above the seam
knows which kernel is underneath.

### Two kernels

`--kernel occt` (default) uses OpenCASCADE. `--kernel smlib` uses
[SMLib](https://github.com/NVIDIA-Omniverse/usd-brep), the NURBS B-Rep kernel NVIDIA
open-sourced under Apache-2.0 in October 2026. It is compiled in only when CMake is
pointed at a usd-brep checkout:

```sh
git clone https://github.com/NVIDIA-Omniverse/usd-brep ../usd-brep
cmake -B build -S . -DOSCAD_WITH_SMLIB=$PWD/../usd-brep   # builds the kernel too (~7 min)
cmake --build build
./build/oscad part.scad --kernel smlib --step part.step
tests/compare-kernels.sh        # same files through both kernels: bbox + volume must agree
```

SMLib has no STEP writer, so its STEP goes through an exact SmBrep→OCCT transfer
(`src/smlib/SmlibToOcct.cc`). The point of a second kernel is a second *oracle*: the
two kernels share no code, so their boolean and fillet failures don't correlate.
`OSCAD_KERNEL=smlib tests/verify-corpus.sh` runs the corpus gate against it. Every
sample and example (primitives, extrudes, booleans, convex/concave fillets, `attach()`)
agrees between the kernels on bounding box and volume. Not yet on smlib: 2D booleans
of overlapping shapes, 2D intersection, mirror of 2D profiles, and exporting a bare 2D
shape (SMLib's sheet bodies crash its tessellator; extrude it instead).

## Build

Requires CMake ≥ 3.16, a C++17 compiler, and the OCCT 7.x dev packages:

```sh
sudo apt-get install -y libocct-foundation-dev libocct-modeling-data-dev \
                        libocct-modeling-algorithms-dev libocct-data-exchange-dev
cmake -B build -S .
cmake --build build
```

## Usage

```sh
oscad input.scad                      # -> input.stl
oscad input.scad -o out.stl           # STL only
oscad input.scad --step out.step      # STEP only
oscad input.scad -o out.stl --step out.step
oscad input.scad -o out.stl --deflection 0.05   # finer tessellation
```

Run the sample suite:

```sh
./tests/run.sh
```

## Verification against the OpenSCAD corpus

We verify differentially against OpenSCAD's own ~580-file `.scad` test corpus and
the reference `openscad` binary. Their golden PNG/STL outputs are kernel-specific
and *not* reused; instead we compare kernel-invariant properties (bounding box;
later volume/topology) between `oscad` (OCCT) and `openscad`. Curved surfaces
diverge slightly because OCCT is exact while OpenSCAD facets — a small delta is
expected; a large one is a real bug (this is how the `cylinder` d-vs-r precedence
bug was caught).

```sh
./tests/fetch-corpus.sh      # downloads corpus to tests/corpus/ (git-ignored, GPL)
./tests/verify-corpus.sh     # differential bbox gate; nonzero exit on a real mismatch
```

The corpus is fetched on demand and git-ignored (OpenSCAD is GPL-2.0+; keeping it
out of this tree preserves clean-room separation). The runner auto-skips files that
use not-yet-implemented features and reports the rest as a scoreboard:

```
in-scope files        : 139     # use only currently-supported features
  matched reference   : 67      # bbox agrees with openscad (21 OCCT-exact on curves)
  unimplemented gaps  : 69      # modifiers * ! # %, include/use, list comprehensions, ...
  expected divergence : 3       # documented allow-list (exact-vs-facet, 2D/3D mixing)
  bbox MISMATCH       : 0
```

As each milestone lands, shrink the `UNSUP` feature regex in `verify-corpus.sh`
and the in-scope count grows — the corpus doubles as a feature-completeness
scoreboard.
