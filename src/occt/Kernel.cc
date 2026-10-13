#include "Kernel.h"

#include <cmath>
#include <memory>
#include <stdexcept>

#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Dir.hxx>
#include <gp_GTrsf.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <gp_Circ.hxx>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakePolygon.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCone.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepBuilderAPI_GTransform.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepOffset_Analyse.hxx>
#include <BRepOffset_Interval.hxx>
#include <BRepOffset_ListOfInterval.hxx>
#include <ChFiDS_TypeOfConcavity.hxx>
#include <GProp_GProps.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Wire.hxx>
#include <string>

namespace {

constexpr double kDeg2Rad = M_PI / 180.0;

TopoDS_Shape applyTrsf(const TopoDS_Shape& s, const gp_Trsf& t) {
  if (s.IsNull()) return s;
  return BRepBuilderAPI_Transform(s, t, /*copy=*/Standard_True).Shape();
}

// Fuse all children into one shape (skipping null/empty subtrees).
TopoDS_Shape unionOf(const std::vector<NodePtr>& kids);  // fwd

// ---- primitives ----
TopoDS_Shape makeCube(const CubeNode& c) {
  if (c.x <= 0 || c.y <= 0 || c.z <= 0) return {};
  double x0 = c.center ? -c.x / 2 : 0;
  double y0 = c.center ? -c.y / 2 : 0;
  double z0 = c.center ? -c.z / 2 : 0;
  return BRepPrimAPI_MakeBox(gp_Pnt(x0, y0, z0), c.x, c.y, c.z).Shape();
}

TopoDS_Shape makeSphere(const SphereNode& s) {
  if (s.r <= 0) return {};
  return BRepPrimAPI_MakeSphere(s.r).Shape();
}

TopoDS_Shape makeCylinder(const CylinderNode& c) {
  if (c.h <= 0 || (c.r1 <= 0 && c.r2 <= 0)) return {};
  double zbase = c.center ? -c.h / 2 : 0;
  gp_Ax2 axis(gp_Pnt(0, 0, zbase), gp_Dir(0, 0, 1));
  if (std::abs(c.r1 - c.r2) < 1e-12)
    return BRepPrimAPI_MakeCylinder(axis, c.r1, c.h).Shape();
  return BRepPrimAPI_MakeCone(axis, c.r1, c.r2, c.h).Shape();
}

// ---- 2D primitives (planar faces in the z=0 plane) ----
TopoDS_Shape faceFromClosedWire(const TopoDS_Wire& w) {
  return BRepBuilderAPI_MakeFace(w, /*OnlyPlane=*/Standard_True).Face();
}

TopoDS_Shape makeSquare(const SquareNode& s) {
  if (s.x <= 0 || s.y <= 0) return {};
  double x0 = s.center ? -s.x / 2 : 0, y0 = s.center ? -s.y / 2 : 0;
  BRepBuilderAPI_MakePolygon poly(gp_Pnt(x0, y0, 0), gp_Pnt(x0 + s.x, y0, 0),
                                  gp_Pnt(x0 + s.x, y0 + s.y, 0), gp_Pnt(x0, y0 + s.y, 0),
                                  Standard_True);
  return faceFromClosedWire(poly.Wire());
}

TopoDS_Shape makeCircle(const CircleNode& c) {
  if (c.r <= 0) return {};
  if (c.fn >= 3) {  // OpenSCAD circle($fn=n) idiom: an exact regular n-gon
    BRepBuilderAPI_MakePolygon poly;
    for (int i = 0; i < c.fn; ++i) {
      double a = 2 * M_PI * i / c.fn;
      poly.Add(gp_Pnt(c.r * std::cos(a), c.r * std::sin(a), 0));
    }
    poly.Close();
    return faceFromClosedWire(poly.Wire());
  }
  gp_Circ circ(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1)), c.r);  // exact circle
  TopoDS_Wire w = BRepBuilderAPI_MakeWire(BRepBuilderAPI_MakeEdge(circ).Edge()).Wire();
  return faceFromClosedWire(w);
}

TopoDS_Shape makePolygon(const PolygonNode& p) {
  if (p.points.size() < 3) return {};
  BRepBuilderAPI_MakePolygon poly;
  for (const auto& pt : p.points) poly.Add(gp_Pnt(pt.first, pt.second, 0));
  poly.Close();
  if (!poly.IsDone()) return {};
  return faceFromClosedWire(poly.Wire());
}

// ---- 2D -> 3D ----
TopoDS_Shape makeLinearExtrude(const LinearExtrudeNode& n) {
  TopoDS_Shape profile = unionOf(n.children);
  if (profile.IsNull() || n.height <= 0) return {};
  TopoDS_Shape solid = BRepPrimAPI_MakePrism(profile, gp_Vec(0, 0, n.height)).Shape();
  if (n.center) {
    gp_Trsf t;
    t.SetTranslation(gp_Vec(0, 0, -n.height / 2));
    solid = applyTrsf(solid, t);
  }
  return solid;
}

TopoDS_Shape makeRotateExtrude(const RotateExtrudeNode& n) {
  TopoDS_Shape profile = unionOf(n.children);
  if (profile.IsNull()) return {};
  // OpenSCAD maps the profile's (x,y) to (radius, height): rotate +90 deg about X
  // so (x,y,0) -> (x,0,y), then revolve about the global Z axis.
  gp_Trsf toXZ;
  toXZ.SetRotation(gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0)), M_PI / 2);
  TopoDS_Shape prof = applyTrsf(profile, toXZ);
  gp_Ax1 axis(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
  if (std::abs(n.angle) >= 360.0) return BRepPrimAPI_MakeRevol(prof, axis).Shape();
  return BRepPrimAPI_MakeRevol(prof, axis, n.angle * kDeg2Rad).Shape();
}

// ---- transforms ----
TopoDS_Shape makeTranslate(const TranslateNode& n) {
  gp_Trsf t;
  t.SetTranslation(gp_Vec(n.x, n.y, n.z));
  return applyTrsf(unionOf(n.children), t);
}

TopoDS_Shape makeRotate(const RotateNode& n) {
  gp_Trsf t;
  gp_Pnt o(0, 0, 0);
  if (n.axisAngle) {
    if (n.ax == 0 && n.ay == 0 && n.az == 0) return unionOf(n.children);  // no axis
    t.SetRotation(gp_Ax1(o, gp_Dir(n.ax, n.ay, n.az)), n.angle * kDeg2Rad);
  } else {
    // Euler: apply X, then Y, then Z  =>  matrix Rz * Ry * Rx.
    gp_Trsf rx, ry, rz;
    rx.SetRotation(gp_Ax1(o, gp_Dir(1, 0, 0)), n.x * kDeg2Rad);
    ry.SetRotation(gp_Ax1(o, gp_Dir(0, 1, 0)), n.y * kDeg2Rad);
    rz.SetRotation(gp_Ax1(o, gp_Dir(0, 0, 1)), n.z * kDeg2Rad);
    t = rz;
    t.Multiply(ry);
    t.Multiply(rx);
  }
  return applyTrsf(unionOf(n.children), t);
}

TopoDS_Shape makeScale(const ScaleNode& n) {
  TopoDS_Shape s = unionOf(n.children);
  if (s.IsNull()) return s;
  gp_GTrsf g;  // identity by default
  g.SetValue(1, 1, n.x);
  g.SetValue(2, 2, n.y);
  g.SetValue(3, 3, n.z);
  return BRepBuilderAPI_GTransform(s, g, /*copy=*/Standard_True).Shape();
}

TopoDS_Shape makeMirror(const MirrorNode& n) {
  if (n.x == 0 && n.y == 0 && n.z == 0) return unionOf(n.children);
  gp_Trsf t;
  t.SetMirror(gp_Ax2(gp_Pnt(0, 0, 0), gp_Dir(n.x, n.y, n.z)));
  return applyTrsf(unionOf(n.children), t);
}

// ---- B-Rep feature layer ----
// Round all edges of the children by radius r. OCCT builds the exact rolling-ball
// blend surfaces and the spherical corner patches where filleted edges meet.
// Fillet is a *partial* operation: too large an r for some face throws -- we
// surface that honestly rather than producing a silently-wrong shape.
TopoDS_Shape makeFillet(const FilletNode& n) {
  TopoDS_Shape s = unionOf(n.children);
  if (s.IsNull() || n.r <= 0) return s;
  TopTools_IndexedMapOfShape edges;
  TopExp::MapShapes(s, TopAbs_EDGE, edges);  // indexed map dedupes shared edges
  if (edges.IsEmpty()) return s;

  // The declarative query: keep an edge iff it matches the selector. For non-"all"
  // selectors we use OCCT's own convex/concave analyser (the classifier its fillet
  // and offset code rely on) rather than a hand-rolled, orientation-fragile test.
  std::unique_ptr<BRepOffset_Analyse> analyse;
  if (n.sel != FilletNode::Sel::All)
    analyse = std::make_unique<BRepOffset_Analyse>(s, /*angleTol=*/0.01);

  auto wanted = [&](const TopoDS_Edge& e) -> bool {
    if (n.sel == FilletNode::Sel::All) return true;
    const BRepOffset_ListOfInterval& iv = analyse->Type(e);
    if (iv.IsEmpty()) return false;
    ChFiDS_TypeOfConcavity t = iv.First().Type();
    return (n.sel == FilletNode::Sel::Convex)  ? (t == ChFiDS_Convex)
         : (n.sel == FilletNode::Sel::Concave) ? (t == ChFiDS_Concave)
                                               : false;
  };

  try {
    BRepFilletAPI_MakeFillet mf(s);
    int added = 0;
    for (int i = 1; i <= edges.Extent(); ++i) {
      TopoDS_Edge e = TopoDS::Edge(edges(i));
      if (wanted(e)) { mf.Add(n.r, e); ++added; }
    }
    if (added == 0) return s;  // nothing matched -> pass the solid through unchanged
    mf.Build();
    if (!mf.IsDone()) throw std::runtime_error("fillet: OCCT could not build the blend");
    return mf.Shape();
  } catch (const Standard_Failure& e) {
    throw std::runtime_error(std::string("fillet failed (r=") + std::to_string(n.r) +
                             " too large for an edge?): " + e.GetMessageString());
  }
}

// ---- assembly: frames / datums (the constructive dual of selection) ----
// A datum frame derived from a face: where it is and how it's oriented. Selection
// asks "which sub-topology do I operate on?"; a frame asks "which sub-topology do I
// measure a coordinate system from, and mate to?" -- both pure queries over topology.
struct Frame {
  gp_Pnt origin;   // face centroid (area centre of mass)
  gp_Dir normal;   // outward normal (accounts for face orientation)
  gp_Dir xdir;     // an in-plane reference axis (for the mate's spin)
  bool   valid = false;
};

// Exact frame of a PLANAR face. Non-planar faces (cylinders, spheres) yield no datum
// here -- a v1 limitation, surfaced honestly by attach() rather than approximated.
Frame frameOfFace(const TopoDS_Face& f) {
  Frame fr;
  BRepAdaptor_Surface surf(f, /*restriction=*/Standard_True);
  if (surf.GetType() != GeomAbs_Plane) return fr;
  gp_Pln pln = surf.Plane();
  gp_Dir n = pln.Axis().Direction();
  if (f.Orientation() == TopAbs_REVERSED) n.Reverse();  // point OUT of the solid
  GProp_GProps props;
  BRepGProp::SurfaceProperties(f, props);
  fr.origin = props.CentreOfMass();
  fr.normal = n;
  fr.xdir   = pln.Position().XDirection();  // guaranteed perpendicular to n
  fr.valid  = true;
  return fr;
}

// Query: the planar face of `s` whose outward normal is closest to `want`.
Frame pickFaceByNormal(const TopoDS_Shape& s, const gp_Dir& want) {
  Frame best;
  double bestDot = -2.0;
  for (TopExp_Explorer ex(s, TopAbs_FACE); ex.More(); ex.Next()) {
    Frame fr = frameOfFace(TopoDS::Face(ex.Current()));
    if (!fr.valid) continue;
    double d = fr.normal.Dot(want);
    if (d > bestDot) { bestDot = d; best = fr; }
  }
  return best;
}

// Rigid transform seating `child`'s frame onto `base`'s. opposed=true => the two
// outward normals end up antiparallel (a face-to-face seat, no interpenetration).
gp_Trsf mateTransform(const Frame& base, const Frame& child, bool opposed) {
  gp_Dir targetN = opposed ? gp_Dir(base.normal.Reversed()) : base.normal;
  gp_Ax3 from(child.origin, child.normal, child.xdir);
  gp_Ax3 to  (base.origin,  targetN,      base.xdir);
  gp_Trsf t;
  t.SetDisplacement(from, to);
  return t;
}

// Direction words name a face by its outward normal, evaluated per-solid in local
// coords -- the positioning-side analogue of fillet's edge predicate.
gp_Dir dirWord(const std::string& w) {
  if (w == "top")    return gp_Dir(0, 0, 1);
  if (w == "bottom") return gp_Dir(0, 0, -1);
  if (w == "right")  return gp_Dir(1, 0, 0);
  if (w == "left")   return gp_Dir(-1, 0, 0);
  if (w == "back")   return gp_Dir(0, 1, 0);
  if (w == "front")  return gp_Dir(0, -1, 0);
  throw std::runtime_error("attach: unknown face \"" + w +
                           "\" (expected top|bottom|left|right|front|back)");
}

std::string oppositeWord(const std::string& w) {
  if (w == "top")    return "bottom";
  if (w == "bottom") return "top";
  if (w == "left")   return "right";
  if (w == "right")  return "left";
  if (w == "front")  return "back";
  if (w == "back")   return "front";
  throw std::runtime_error("attach: unknown face \"" + w + "\"");
}

TopoDS_Shape makeAttach(const AttachNode& n) {
  if (n.children.empty()) return {};
  TopoDS_Shape parent = buildShape(*n.children[0]);
  if (parent.IsNull() || n.children.size() == 1) return parent;  // nothing to seat

  constexpr double kFacing = 0.5;  // picked face must clearly face the requested dir
  gp_Dir onDir = dirWord(n.on);
  std::string fromWord = n.from.empty() ? oppositeWord(n.on) : n.from;
  gp_Dir fromDir = dirWord(fromWord);

  Frame pf = pickFaceByNormal(parent, onDir);
  if (!pf.valid || pf.normal.Dot(onDir) < kFacing)
    throw std::runtime_error("attach(on=\"" + n.on +
                             "\"): parent has no planar face clearly facing \"" + n.on + "\"");

  TopoDS_Shape acc = parent;
  for (size_t i = 1; i < n.children.size(); ++i) {
    TopoDS_Shape child = buildShape(*n.children[i]);
    if (child.IsNull()) continue;
    Frame cf = pickFaceByNormal(child, fromDir);
    if (!cf.valid || cf.normal.Dot(fromDir) < kFacing)
      throw std::runtime_error("attach(from=\"" + fromWord +
                               "\"): a seated child has no planar face facing \"" + fromWord + "\"");
    TopoDS_Shape placed = applyTrsf(child, mateTransform(pf, cf, /*opposed=*/true));
    acc = BRepAlgoAPI_Fuse(acc, placed).Shape();
  }
  return acc;
}

// ---- booleans ----
TopoDS_Shape makeDifference(const std::vector<NodePtr>& kids) {
  TopoDS_Shape first;
  size_t i = 0;
  for (; i < kids.size(); ++i) {
    first = buildShape(*kids[i]);
    if (!first.IsNull()) { ++i; break; }
  }
  if (first.IsNull()) return first;
  TopoDS_Shape tools;
  for (; i < kids.size(); ++i) {
    TopoDS_Shape s = buildShape(*kids[i]);
    if (s.IsNull()) continue;
    tools = tools.IsNull() ? s : BRepAlgoAPI_Fuse(tools, s).Shape();
  }
  if (tools.IsNull()) return first;
  return BRepAlgoAPI_Cut(first, tools).Shape();
}

TopoDS_Shape makeIntersection(const std::vector<NodePtr>& kids) {
  TopoDS_Shape acc;
  for (const auto& c : kids) {
    TopoDS_Shape s = buildShape(*c);
    if (s.IsNull()) return {};  // intersection with empty is empty
    acc = acc.IsNull() ? s : BRepAlgoAPI_Common(acc, s).Shape();
  }
  return acc;
}

}  // namespace

namespace {
TopoDS_Shape unionOf(const std::vector<NodePtr>& kids) {
  TopoDS_Shape acc;
  for (const auto& c : kids) {
    TopoDS_Shape s = buildShape(*c);
    if (s.IsNull()) continue;
    acc = acc.IsNull() ? s : BRepAlgoAPI_Fuse(acc, s).Shape();
  }
  return acc;
}
}  // namespace

TopoDS_Shape buildShape(const Node& node) {
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
  throw std::runtime_error("unhandled node kind");
}
