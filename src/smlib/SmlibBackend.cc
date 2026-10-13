// SMLib backend: the Node tree -> SM_API calls. Same semantics as the OCCT
// backend (see occt/Kernel.cc), so the differential corpus gate can run both.
//
// Representation: a 3D result is an SmBrep solid. A 2D result (square, circle,
// polygon and their transforms/booleans) is a flat list of closed planar curves
// in z=0 that an extrude consumes. SM_API booleans are in-place and consume the
// second operand on success, so ownership is tracked explicitly in Geo.
#include "SmlibBackend.h"

#include <SmApiBrep.h>
#include <SmApiCurves.h>
#include <SmApiFillets.h>
#include <SmApiGeneral.h>
#include <SmApiPrimitives.h>
#include <SmApiQueries.h>
#include <SmApiTessellationParams.h>
#include <SmAxis2Placement.h>
#include <SmBSplineCurve.h>
#include <SmBrep.h>
#include <SmCurve.h>
#include <SmEdge.h>
#include <SmEdgeuse.h>
#include <SmFace.h>
#include <SmFaceuse.h>
#include <SmPlane.h>
#include <SmRegion.h>
#include <SmLine.h>
#include <SmPoly.h>
#include <SmTArray.h>
#include <SmVector3d.h>

#include <Standard_Failure.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "SmlibToOcct.h"
#include "occt/Export.h"

namespace {

[[noreturn]] void fail(const std::string& m) { throw std::runtime_error("smlib: " + m); }
void check(SmApiStatus st, const char* what) {
  if (st != SM_SUCCESS) fail(std::string(what) + " failed (status " + std::to_string(st) + ")");
}

// Owning handle for either a 3D solid or a 2D profile (closed curves in z=0).
struct Geo {
  SmBrep* brep = nullptr;
  std::vector<SmCurve*> curves;

  Geo() = default;
  explicit Geo(SmBrep* b) : brep(b) {}
  Geo(const Geo&) = delete;
  Geo& operator=(const Geo&) = delete;
  Geo(Geo&& o) noexcept { *this = std::move(o); }
  Geo& operator=(Geo&& o) noexcept {
    if (this != &o) { reset(); brep = o.brep; curves = std::move(o.curves); o.brep = nullptr; o.curves.clear(); }
    return *this;
  }
  ~Geo() { reset(); }
  void reset() {
    delete brep;
    brep = nullptr;
    for (SmCurve* c : curves) delete c;
    curves.clear();
  }
  SmBrep* releaseBrep() { SmBrep* b = brep; brep = nullptr; return b; }
  bool is2d() const { return !curves.empty(); }
  bool empty() const { return brep == nullptr && curves.empty(); }
  SmTArray<SmCurve*> curveArray() const {
    SmTArray<SmCurve*> a;
    for (SmCurve* c : curves) a.Add(c);
    return a;
  }
};

Geo build(const Node& node);

// OpenSCAD treats non-positive or non-finite dimensions as "nothing" (circle(1/0),
// cube(0)); SM_API rejects them with an error, so filter first.
bool usable(double d) { return std::isfinite(d) && d > 0; }

// SMLib booleans can return SM_SUCCESS and silently drop an operand on degenerate
// coincidences (seen: a revolve touching its axis unioned with a box whose face
// lies in the axis plane -- the ring vanished). Volume is cheap to check, so check
// it: fail loud rather than export a wrong solid.
// A boolean that removes everything yields a body with no faces, not an error.
bool isEmptyBrep(SmBrep* b) {
  if (!b) return true;
  SmTArray<SmFace*> faces;
  return SmApiGetFaces(b, faces) != SM_SUCCESS || faces.GetSize() == 0;
}
void dropIfEmpty(Geo& g) {
  if (g.brep && isEmptyBrep(g.brep)) g.reset();
}

double volumeOf(SmBrep* b) {
  double v = 0;
  return SmApiBrepComputeVolume(b, 1e-3, v) == SM_SUCCESS ? v : -1.0;
}
void requireVolume(const char* op, double got, double atLeast) {
  if (got < 0 || atLeast < 0) return;  // could not measure: nothing to say
  if (got < atLeast * (1.0 - 1e-2))
    fail(std::string(op) + " dropped geometry: result volume " + std::to_string(got) +
         " < operand volume " + std::to_string(atLeast) + " (kernel reported success)");
}

Geo unionOf(const std::vector<NodePtr>& kids) {
  Geo acc;
  for (const auto& k : kids) {
    Geo g = build(*k);
    if (g.empty()) continue;
    if (acc.empty()) { acc = std::move(g); continue; }
    if (acc.is2d() != g.is2d()) fail("union mixes 2D and 3D children");
    if (acc.is2d()) {
      // 2D union = all loops together. Correct for disjoint shapes; overlapping
      // 2D shapes would need SmApiBoolean2d on planar faces (not wired yet).
      acc.curves.insert(acc.curves.end(), g.curves.begin(), g.curves.end());
      g.curves.clear();
      continue;
    }
    double va = volumeOf(acc.brep), vb = volumeOf(g.brep);
    SmBrep* res = nullptr;
    check(SmApiBooleanUnion(acc.brep, g.brep, res), "SmApiBooleanUnion");
    g.releaseBrep();  // consumed by the kernel on success
    requireVolume("union", volumeOf(acc.brep), std::max(va, vb));
  }
  dropIfEmpty(acc);
  return acc;
}

Geo makeDifference(const std::vector<NodePtr>& kids) {
  if (kids.empty()) return {};
  Geo acc = build(*kids[0]);
  if (acc.empty()) return {};
  for (size_t i = 1; i < kids.size(); ++i) {
    Geo g = build(*kids[i]);
    if (g.empty()) continue;
    if (acc.is2d() != g.is2d()) fail("difference mixes 2D and 3D children");
    if (acc.is2d()) {
      // 2D difference = holes: assumes the tool lies inside the outer loop.
      acc.curves.insert(acc.curves.end(), g.curves.begin(), g.curves.end());
      g.curves.clear();
      continue;
    }
    SmBrep* res = nullptr;
    check(SmApiBooleanDifference(acc.brep, g.brep, res), "SmApiBooleanDifference");
    g.releaseBrep();
    dropIfEmpty(acc);
    if (acc.empty()) return {};
  }
  return acc;
}

Geo makeIntersection(const std::vector<NodePtr>& kids) {
  if (kids.empty()) return {};
  Geo acc = build(*kids[0]);
  if (acc.empty()) return {};
  for (size_t i = 1; i < kids.size(); ++i) {
    Geo g = build(*kids[i]);
    if (g.empty()) return {};  // intersecting with nothing is nothing
    if (acc.is2d() || g.is2d()) fail("2D intersection not supported by the smlib backend yet");
    SmBrep* res = nullptr;
    check(SmApiBooleanIntersection(acc.brep, g.brep, res), "SmApiBooleanIntersection");
    g.releaseBrep();
    dropIfEmpty(acc);
    if (acc.empty()) return {};
  }
  return acc;
}

// ---- 3D primitives ----
Geo makeCube(const CubeNode& c) {
  if (!usable(c.x) || !usable(c.y) || !usable(c.z)) return {};
  SmVector3d o = c.center ? SmVector3d(-c.x / 2, -c.y / 2, -c.z / 2) : SmVector3d(0, 0, 0);
  SmBrep* b = nullptr;
  check(SmApiCreateBox(o, c.x, c.y, c.z, b), "SmApiCreateBox");
  return Geo(b);
}

Geo makeSphere(const SphereNode& s) {
  if (!usable(s.r)) return {};
  SmBrep* b = nullptr;
  // Pole-free: a sphere with poles survives SMLib fine but its pole face does
  // not cross the STEP bridge cleanly (see SmlibToOcct.h).
  check(SmApiCreateSphereNoPole(SmVector3d(0, 0, 0), s.r, b), "SmApiCreateSphereNoPole");
  return Geo(b);
}

Geo makeCylinder(const CylinderNode& c) {
  if (!usable(c.h) || !std::isfinite(c.r1) || !std::isfinite(c.r2) || (c.r1 <= 0 && c.r2 <= 0)) return {};
  SmVector3d o(0, 0, c.center ? -c.h / 2 : 0);
  SmBrep* b = nullptr;
  if (c.r1 == c.r2) check(SmApiCreateCylinder(o, c.r1, c.h, b), "SmApiCreateCylinder");
  else check(SmApiCreateCone(o, c.r1, c.r2, c.h, b), "SmApiCreateCone");
  return Geo(b);
}

// ---- 2D primitives: closed loops of line segments / an exact circle ----
Geo loopFromPoints(const std::vector<std::pair<double, double>>& pts) {
  if (pts.size() < 3) return {};
  Geo g;
  for (size_t i = 0; i < pts.size(); ++i) {
    const auto& a = pts[i];
    const auto& b = pts[(i + 1) % pts.size()];
    if (a == b) continue;
    SmLine* l = nullptr;
    check(SmApiCreateLineSegment(SmPoint3d(a.first, a.second, 0), SmPoint3d(b.first, b.second, 0), l),
          "SmApiCreateLineSegment");
    g.curves.push_back(l);
  }
  return g;
}

Geo makeSquare(const SquareNode& s) {
  if (!usable(s.x) || !usable(s.y)) return {};
  double x0 = s.center ? -s.x / 2 : 0, y0 = s.center ? -s.y / 2 : 0;
  return loopFromPoints({{x0, y0}, {x0 + s.x, y0}, {x0 + s.x, y0 + s.y}, {x0, y0 + s.y}});
}

Geo makeCircle(const CircleNode& c) {
  if (!usable(c.r)) return {};
  if (c.fn >= 3) {  // OpenSCAD circle($fn=n) idiom: an exact regular n-gon
    std::vector<std::pair<double, double>> pts;
    for (int i = 0; i < c.fn; ++i) {
      double a = 2 * M_PI * i / c.fn;
      pts.push_back({c.r * std::cos(a), c.r * std::sin(a)});
    }
    return loopFromPoints(pts);
  }
  SmBSplineCurve* circ = nullptr;
  check(SmApiCreateCircle(SmPoint3d(0, 0, 0), c.r, circ), "SmApiCreateCircle");
  Geo g;
  g.curves.push_back(circ);
  return g;
}

Geo makePolygon(const PolygonNode& p) { return loopFromPoints(p.points); }

// ---- 2D -> 3D ----
Geo makeLinearExtrude(const LinearExtrudeNode& n) {
  Geo p = unionOf(n.children);
  if (p.empty() || n.height <= 0) return {};
  if (!p.is2d()) fail("linear_extrude of a 3D child");
  SmBrep* b = nullptr;
  check(SmApiCreateLinearSweep(p.curveArray(), SmVector3d(0, 0, 1), n.height, TRUE, b), "SmApiCreateLinearSweep");
  Geo g(b);
  if (n.center) check(SmApiTranslate(b, SmVector3d(0, 0, -n.height / 2)), "SmApiTranslate");
  return g;
}

Geo makeRotateExtrude(const RotateExtrudeNode& n) {
  Geo p = unionOf(n.children);
  if (p.empty()) return {};
  if (!p.is2d()) fail("rotate_extrude of a 3D child");
  // OpenSCAD maps the profile's (x,y) to (radius, height): rotate +90 deg about X
  // so (x,y,0) -> (x,0,y), then revolve about the global Z axis.
  for (SmCurve* c : p.curves)
    check(SmApiRotate(c, SmVector3d(0, 0, 0), SmVector3d(1, 0, 0), 90.0), "SmApiRotate");
  double ang = std::abs(n.angle) >= 360.0 ? 360.0 : n.angle;
  SmBrep* b = nullptr;
  check(SmApiCreateRotationalSweep(p.curveArray(), SmPoint3d(0, 0, 0), SmVector3d(0, 0, 1), ang, TRUE, b),
        "SmApiCreateRotationalSweep");
  return Geo(b);
}

// ---- transforms (apply to the solid, or to every profile curve) ----
template <typename F>
Geo transformed(const std::vector<NodePtr>& kids, F&& f) {
  Geo g = unionOf(kids);
  if (g.empty()) return g;
  if (g.brep) f(static_cast<SmObject*>(g.brep));
  for (SmCurve* c : g.curves) f(static_cast<SmObject*>(c));
  return g;
}

Geo makeTranslate(const TranslateNode& n) {
  return transformed(n.children, [&](SmObject* o) {
    check(SmApiTranslate(o, SmVector3d(n.x, n.y, n.z)), "SmApiTranslate");
  });
}

Geo makeRotate(const RotateNode& n) {
  const SmVector3d o(0, 0, 0);
  if (n.axisAngle) {
    if (n.ax == 0 && n.ay == 0 && n.az == 0) return unionOf(n.children);
    return transformed(n.children, [&](SmObject* obj) {
      check(SmApiRotate(obj, o, SmVector3d(n.ax, n.ay, n.az), n.angle), "SmApiRotate");
    });
  }
  // Euler: apply X, then Y, then Z  =>  matrix Rz * Ry * Rx (same as occt/Kernel.cc).
  return transformed(n.children, [&](SmObject* obj) {
    if (n.x != 0) check(SmApiRotate(obj, o, SmVector3d(1, 0, 0), n.x), "SmApiRotate");
    if (n.y != 0) check(SmApiRotate(obj, o, SmVector3d(0, 1, 0), n.y), "SmApiRotate");
    if (n.z != 0) check(SmApiRotate(obj, o, SmVector3d(0, 0, 1), n.z), "SmApiRotate");
  });
}

Geo makeScale(const ScaleNode& n) {
  return transformed(n.children, [&](SmObject* o) {
    check(SmApiScale(o, SmVector3d(n.x, n.y, n.z)), "SmApiScale");
  });
}

Geo makeMirror(const MirrorNode& n) {
  if (n.x == 0 && n.y == 0 && n.z == 0) return unionOf(n.children);
  Geo g = unionOf(n.children);
  if (g.empty()) return g;
  if (g.is2d()) fail("mirror of a 2D profile not supported by the smlib backend yet");
  double len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
  SmVector3d nrm(n.x / len, n.y / len, n.z / len);
  if (g.brep->Mirror(SmAxis2Placement(nrm, 0.0)) != SM_SUCCESS) fail("SmBrep::Mirror failed");
  return g;
}

// ---- B-Rep feature layer ----
Geo makeFillet(const FilletNode& n) {
  Geo g = unionOf(n.children);
  if (g.empty()) return g;
  if (g.is2d()) fail("fillet of a 2D profile");
  if (n.sel == FilletNode::Sel::All) {
    check(SmApiCircularFillet(g.brep, n.r), "SmApiCircularFillet");
    return g;
  }
  // Convex/concave selection by the edge's outer dihedral angle at its
  // mid-parameter, measured OUTSIDE the material: a cube edge reads 270 deg, a
  // reentrant edge 90 deg, a tangent (smooth) edge 180 deg.
  SmTArray<SmEdge*> edges, sel;
  check(SmApiGetEdges(g.brep, edges), "SmApiGetEdges");
  for (ULONG i = 0; i < edges.GetSize(); ++i) {
    SmEdge* e = edges.GetAt(i);
    double a = 0, b = 0, ang = 0;
    if (SmApiEdgeParameterRange(e, a, b) != SM_SUCCESS) continue;
    if (e->CalcOuterAngle((a + b) / 2, ang) != SM_SUCCESS) continue;
    if (std::getenv("OSCAD_SMLIB_DEBUG")) {
      SmPoint3d p0, p1;
      SmApiVertexGetPoint(e->GetStartVertex(), p0);
      SmApiVertexGetPoint(e->GetEndVertex(), p1);
      std::cerr << "smlib: edge#" << i << " outer_angle_deg=" << ang * 180.0 / M_PI << " from (" << p0[0] << "," << p0[1] << "," << p0[2]
                << ") to (" << p1[0] << "," << p1[1] << "," << p1[2] << ")\n";
    }
    const double tol = 1e-4;
    bool convex = ang > M_PI + tol, concave = ang > tol && ang < M_PI - tol;  // ~180: tangent, skip
    if ((n.sel == FilletNode::Sel::Convex && convex) || (n.sel == FilletNode::Sel::Concave && concave)) sel.Add(e);
  }
  if (sel.GetSize() == 0) return g;
  check(SmApiFilletEdges(g.brep, sel, n.r, /*xsect=*/1 /*circular*/, /*continuity=*/1, /*thumbweight=*/1.0),
        "SmApiFilletEdges");
  return g;
}

// ---- assembly: frames / datums (mirror of occt/Kernel.cc makeAttach) ----
struct Frame {
  SmPoint3d origin;   // face centroid
  SmVector3d normal;  // outward normal
  SmVector3d xdir;    // in-plane reference axis
  bool valid = false;
};

// Exact frame of a PLANAR face. The upward faceuse's normal equals the surface
// normal by SMLib convention; it points outward iff that faceuse borders the
// infinite region.
Frame frameOfFace(SmFace* f) {
  Frame fr;
  SmSurface* srf = f->GetSurface();
  if (!srf || !srf->IsKindOf(SmPlane_TYPE)) return fr;
  const SmAxis2Placement& pos = static_cast<const SmPlane*>(srf)->GetPosition();
  SmVector3d n = pos.GetZAxis();
  SmFaceuse* fu = f->GetUpwardFaceuse();
  SmRegion* reg = fu ? fu->GetRegion() : nullptr;
  if (!(reg && reg->IsInfiniteRegion())) n = n * -1.0;
  double area = 0;
  if (SmApiFaceComputeCentroid(f, 1e-4, area, fr.origin) != SM_SUCCESS) return fr;
  fr.normal = n;
  fr.xdir = pos.GetXAxis();
  fr.valid = true;
  return fr;
}

Frame pickFaceByNormal(SmBrep* b, const SmVector3d& want) {
  Frame best;
  double bestDot = -2.0;
  SmTArray<SmFace*> faces;
  check(SmApiGetFaces(b, faces), "SmApiGetFaces");
  for (ULONG i = 0; i < faces.GetSize(); ++i) {
    Frame fr = frameOfFace(faces.GetAt(i));
    if (!fr.valid) continue;
    double d = fr.normal.Dot(want);
    if (d > bestDot) { bestDot = d; best = fr; }
  }
  return best;
}

SmVector3d dirWord(const std::string& w) {
  if (w == "top")    return SmVector3d(0, 0, 1);
  if (w == "bottom") return SmVector3d(0, 0, -1);
  if (w == "right")  return SmVector3d(1, 0, 0);
  if (w == "left")   return SmVector3d(-1, 0, 0);
  if (w == "back")   return SmVector3d(0, 1, 0);
  if (w == "front")  return SmVector3d(0, -1, 0);
  fail("attach: unknown face \"" + w + "\" (expected top|bottom|left|right|front|back)");
}

std::string oppositeWord(const std::string& w) {
  if (w == "top")    return "bottom";
  if (w == "bottom") return "top";
  if (w == "left")   return "right";
  if (w == "right")  return "left";
  if (w == "front")  return "back";
  if (w == "back")   return "front";
  fail("attach: unknown face \"" + w + "\"");
}

Geo makeAttach(const AttachNode& n) {
  if (n.children.empty()) return {};
  Geo parent = build(*n.children[0]);
  if (parent.empty() || n.children.size() == 1) return parent;
  if (parent.is2d()) fail("attach: parent must be a solid");
  constexpr double kFacing = 0.5;
  SmVector3d onDir = dirWord(n.on);
  std::string fromWord = n.from.empty() ? oppositeWord(n.on) : n.from;
  SmVector3d fromDir = dirWord(fromWord);
  Frame pf = pickFaceByNormal(parent.brep, onDir);
  if (!pf.valid || pf.normal.Dot(onDir) < kFacing)
    fail("attach(on=\"" + n.on + "\"): parent has no planar face clearly facing \"" + n.on + "\"");
  for (size_t i = 1; i < n.children.size(); ++i) {
    Geo child = build(*n.children[i]);
    if (child.empty()) continue;
    if (child.is2d()) fail("attach: children must be solids");
    Frame cf = pickFaceByNormal(child.brep, fromDir);
    if (!cf.valid || cf.normal.Dot(fromDir) < kFacing)
      fail("attach(from=\"" + fromWord + "\"): a seated child has no planar face facing \"" + fromWord + "\"");
    // Seat child frame onto the parent frame with normals opposed (face-to-face).
    SmVector3d toZ = pf.normal * -1.0;
    SmVector3d toX = pf.xdir, toY = toZ * toX;      // '*' on SmVector3d is the cross product
    SmVector3d fromX = cf.xdir, fromY = cf.normal * fromX;
    SmAxis2Placement mate(cf.origin, fromX, fromY, pf.origin, toX, toY);
    check(SmApiTransform(child.brep, mate), "SmApiTransform");
    SmBrep* res = nullptr;
    check(SmApiBooleanUnion(parent.brep, child.brep, res), "SmApiBooleanUnion");
    child.releaseBrep();
  }
  return parent;
}

Geo build(const Node& node) {
  switch (node.kind) {
    case NodeKind::Cube:      return makeCube(static_cast<const CubeNode&>(node));
    case NodeKind::Sphere:    return makeSphere(static_cast<const SphereNode&>(node));
    case NodeKind::Cylinder:  return makeCylinder(static_cast<const CylinderNode&>(node));
    case NodeKind::Square:    return makeSquare(static_cast<const SquareNode&>(node));
    case NodeKind::Circle:    return makeCircle(static_cast<const CircleNode&>(node));
    case NodeKind::Polygon:   return makePolygon(static_cast<const PolygonNode&>(node));
    case NodeKind::LinearExtrude: return makeLinearExtrude(static_cast<const LinearExtrudeNode&>(node));
    case NodeKind::RotateExtrude: return makeRotateExtrude(static_cast<const RotateExtrudeNode&>(node));
    case NodeKind::Translate: return makeTranslate(static_cast<const TranslateNode&>(node));
    case NodeKind::Rotate:    return makeRotate(static_cast<const RotateNode&>(node));
    case NodeKind::Scale:     return makeScale(static_cast<const ScaleNode&>(node));
    case NodeKind::Mirror:    return makeMirror(static_cast<const MirrorNode&>(node));
    case NodeKind::Fillet:    return makeFillet(static_cast<const FilletNode&>(node));
    case NodeKind::Attach:    return makeAttach(static_cast<const AttachNode&>(node));
    case NodeKind::Union:
    case NodeKind::Group:        return unionOf(node.children);
    case NodeKind::Difference:   return makeDifference(node.children);
    case NodeKind::Intersection: return makeIntersection(node.children);
  }
  fail("unknown node kind");
}

// ---- output ----
struct Tri { float n[3], a[3], b[3], c[3]; };

void pushTri(std::vector<Tri>& out, const SmPoint3d& p, const SmPoint3d& q, const SmPoint3d& r) {
  double ux = q[0] - p[0], uy = q[1] - p[1], uz = q[2] - p[2];
  double vx = r[0] - p[0], vy = r[1] - p[1], vz = r[2] - p[2];
  double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
  double len = std::sqrt(nx * nx + ny * ny + nz * nz);
  if (len < 1e-18) return;  // degenerate sliver
  Tri t;
  t.n[0] = (float)(nx / len); t.n[1] = (float)(ny / len); t.n[2] = (float)(nz / len);
  for (int i = 0; i < 3; ++i) { t.a[i] = (float)p[i]; t.b[i] = (float)q[i]; t.c[i] = (float)r[i]; }
  out.push_back(t);
}

bool writeBinaryStl(const std::vector<Tri>& tris, const std::string& path) {
  std::ofstream out(path, std::ios::binary);
  if (!out) return false;
  char header[80] = "oscad smlib backend";
  out.write(header, 80);
  uint32_t n = (uint32_t)tris.size();
  out.write(reinterpret_cast<const char*>(&n), 4);
  const uint16_t attr = 0;
  for (const Tri& t : tris) {
    out.write(reinterpret_cast<const char*>(t.n), 12);
    out.write(reinterpret_cast<const char*>(t.a), 12);
    out.write(reinterpret_cast<const char*>(t.b), 12);
    out.write(reinterpret_cast<const char*>(t.c), 12);
    out.write(reinterpret_cast<const char*>(&attr), 2);
  }
  return static_cast<bool>(out);
}

struct SmlibSolid : Solid {
  SmBrep* brep;
  explicit SmlibSolid(SmBrep* b) : brep(b) {}
  ~SmlibSolid() override { delete brep; }

  bool writeStl(const std::string& path, double linearDeflection) override {
    SmTessellationParams prm;
    prm.dChordHeightTol = linearDeflection;
    prm.dSurfaceAngleTolDeg = 15.0;  // upstream KNOWN_ISSUES: avoids inside-out triangles near fillets
    prm.dCurveAngleTolDeg = 15.0;
    SmPolyBrep* pb = nullptr;
    if (SmApiTessellate(brep, pb, prm) != SM_SUCCESS || pb == nullptr) return false;
    std::vector<Tri> tris;
    SmTArray<SmPolyFace*> faces;
    pb->GetPolyFaces(faces);
    for (ULONG i = 0; i < faces.GetSize(); ++i) {
      SmPolyLoop* loop = faces.GetAt(i)->GetOuterPolyLoop();
      if (!loop) continue;
      SmTArray<SmPolyEdge*> edges;
      loop->GetPolyEdges(edges);
      std::vector<SmPoint3d> pts;
      for (ULONG j = 0; j < edges.GetSize(); ++j) pts.push_back(edges.GetAt(j)->GetStartPoint());
      for (size_t j = 1; j + 1 < pts.size(); ++j) pushTri(tris, pts[0], pts[j], pts[j + 1]);  // fan
    }
    delete pb;
    return writeBinaryStl(tris, path);
  }

  bool writeStep(const std::string& path) override {
    try {
      TopoDS_Shape s = smlibToOcct(brep);
      return ::writeStep(s, path);
    } catch (const Standard_Failure& e) {
      throw std::runtime_error(std::string("smlib->occt: OCCT: ") + e.GetMessageString());
    }
  }
};

struct SmlibBackend : Backend {
  SmlibBackend() {
    static std::once_flag once;
    std::call_once(once, [] { SmApiCreateContext(); });
  }
  const char* name() const override { return "smlib"; }
  std::unique_ptr<Solid> build(const Node& root) override {
    Geo g = ::build(root);
    if (g.empty()) return nullptr;
    // Top-level 2D (a flat face, which the OCCT backend exports as a zero-volume
    // mesh) is refused here: the sheet body from SmApiCreatePlanarFaces crashes
    // SMLib's tessellator (null call in SmBrep::MergeBrep) and our STEP bridge.
    // Fail loud rather than segfault.
    if (g.is2d()) fail("top-level 2D geometry is not exportable on the smlib backend (extrude it)");
    dropIfEmpty(g);
    if (g.empty()) return nullptr;
    if (std::getenv("OSCAD_SMLIB_DEBUG")) {
      SmBoolean manifold = FALSE;
      double vol = 0;
      SmApiBrepIsManifoldSolid(g.brep, manifold);
      SmApiBrepComputeVolume(g.brep, 1e-7, vol);
      SmTArray<SmFace*> faces;
      SmApiGetFaces(g.brep, faces);
      std::cerr << "smlib: result manifold_solid=" << (manifold ? "YES" : "NO") << " faces=" << faces.GetSize()
                << " kernel_volume=" << vol << "\n";
    }
    return std::make_unique<SmlibSolid>(g.releaseBrep());
  }
};

}  // namespace

std::unique_ptr<Backend> makeSmlibBackend() { return std::make_unique<SmlibBackend>(); }
