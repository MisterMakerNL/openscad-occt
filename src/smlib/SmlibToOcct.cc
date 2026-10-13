#include "SmlibToOcct.h"

#include <SmApiQueries.h>
#include <SmBSplineCurve.h>
#include <SmBSplineSurface.h>
#include <SmBrep.h>
#include <SmEdge.h>
#include <SmEdgeuse.h>
#include <SmFace.h>
#include <SmFaceuse.h>
#include <SmCone.h>
#include <SmCylinder.h>
#include <SmLoopuse.h>
#include <SmPlane.h>
#include <SmSphere.h>
#include <SmTorus.h>
#include <SmTArray.h>
#include <SmVector3d.h>
#include <SmVertex.h>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Solid.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_Array2OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(std::string("smlib->occt: ") + what);
}

// Flat knot vector -> OCCT (unique knots, multiplicities). A vector short by
// one at each end (ncp+deg-1 style) is padded to the clamped equivalent.
struct Knots {
  TColStd_Array1OfReal k;
  TColStd_Array1OfInteger m;
  Knots(const std::vector<double>& uk, const std::vector<int>& um)
      : k(1, (int)uk.size()), m(1, (int)um.size()) {
    for (size_t i = 0; i < uk.size(); ++i) { k((int)i + 1) = uk[i]; m((int)i + 1) = um[i]; }
  }
};

Knots collapse(const std::vector<double>& flat, int ncp, int deg) {
  std::vector<double> uk;
  std::vector<int> um;
  for (double v : flat) {
    if (!uk.empty() && std::abs(v - uk.back()) < 1e-12) um.back()++;
    else { uk.push_back(v); um.push_back(1); }
  }
  int sum = 0;
  for (int x : um) sum += x;
  if (sum == ncp + deg - 1) { um.front()++; um.back()++; sum += 2; }
  require(sum == ncp + deg + 1, "knot count does not match control points + degree + 1");
  require(um.front() == deg + 1 && um.back() == deg + 1, "unclamped knot vector");
  return Knots(uk, um);
}

Handle(Geom_BSplineCurve) toOcct(const SmBSplineCurve& c) {
  const int deg = (int)c.GetDegree();
  const int ncp = (int)c.GetNumberControlPoints();
  SmTArray<double> flat;
  require(c.GetKnotsAll(flat) == SM_SUCCESS, "GetKnotsAll failed");
  std::vector<double> fk;
  for (ULONG i = 0; i < flat.GetSize(); ++i) fk.push_back(flat[i]);
  Knots kn = collapse(fk, ncp, deg);
  TColgp_Array1OfPnt poles(1, ncp);
  TColStd_Array1OfReal w(1, ncp);
  for (int i = 0; i < ncp; ++i) {
    SmPoint3d p;
    double wi = 1.0;
    require(c.GetControlPoint(SM_CP_EUCLIDIAN_RATIONAL, (ULONG)i, p, wi) == SM_SUCCESS, "curve control point");
    poles(i + 1) = gp_Pnt(p[0], p[1], p[2]);
    w(i + 1) = wi;
  }
  if (c.IsRational() == TRUE) return new Geom_BSplineCurve(poles, w, kn.k, kn.m, deg);
  return new Geom_BSplineCurve(poles, kn.k, kn.m, deg);
}

Handle(Geom_BSplineSurface) toOcct(const SmBSplineSurface& s) {
  const int du = (int)s.GetDegree(SM_SP_U), dv = (int)s.GetDegree(SM_SP_V);
  const int nu = (int)s.GetNumberControlPoints(SM_SP_U), nv = (int)s.GetNumberControlPoints(SM_SP_V);
  auto knotsOf = [&](SmSurfParamType dir, int ncp, int deg) {
    SmTArray<double> uk;
    SmTArray<ULONG> um;
    require(s.GetKnots(dir, uk, &um) == SM_SUCCESS, "surface GetKnots failed");
    std::vector<double> flat;
    for (ULONG i = 0; i < uk.GetSize(); ++i)
      for (ULONG r = 0; r < um[i]; ++r) flat.push_back(uk[i]);
    return collapse(flat, ncp, deg);
  };
  Knots ku = knotsOf(SM_SP_U, nu, du);
  Knots kv = knotsOf(SM_SP_V, nv, dv);
  TColgp_Array2OfPnt poles(1, nu, 1, nv);
  TColStd_Array2OfReal w(1, nu, 1, nv);
  for (int i = 0; i < nu; ++i)
    for (int j = 0; j < nv; ++j) {
      SmPoint3d p;
      double wi = 1.0;
      require(s.GetControlPoint(SM_CP_EUCLIDIAN_RATIONAL, (ULONG)i, (ULONG)j, p, wi) == SM_SUCCESS, "surface control point");
      poles(i + 1, j + 1) = gp_Pnt(p[0], p[1], p[2]);
      w(i + 1, j + 1) = wi;
    }
  if (s.IsRational() == TRUE) return new Geom_BSplineSurface(poles, w, ku.k, kv.k, ku.m, kv.m, du, dv);
  return new Geom_BSplineSurface(poles, ku.k, kv.k, ku.m, kv.m, du, dv);
}

}  // namespace

TopoDS_Shape smlibToOcct(SmBrep* brep) {
  require(brep != nullptr, "null brep");
  const bool debug = std::getenv("OSCAD_SMLIB_DEBUG") != nullptr;
  SmTArray<SmFace*> faces;
  require(SmApiGetFaces(brep, faces) == SM_SUCCESS, "SmApiGetFaces failed");

  BRep_Builder B;
  std::map<SmVertex*, TopoDS_Vertex> vmap;
  std::map<SmEdge*, TopoDS_Edge> emap;

  auto vertexOf = [&](SmVertex* v) {
    auto it = vmap.find(v);
    if (it != vmap.end()) return it->second;
    SmPoint3d p;
    require(SmApiVertexGetPoint(v, p) == SM_SUCCESS, "SmApiVertexGetPoint failed");
    TopoDS_Vertex tv = BRepBuilderAPI_MakeVertex(gp_Pnt(p[0], p[1], p[2]));
    vmap[v] = tv;
    return tv;
  };
  // One OCCT edge per SMLib edge, bounded by SMLib's shared vertices; vertex
  // tolerance grows to cover any gap between curve end and vertex.
  auto edgeOf = [&](SmEdge* e) {
    auto it = emap.find(e);
    if (it != emap.end()) return it->second;
    SmCurve* c = e->GetCurve();
    require(c && c->IsKindOf(SmBSplineCurve_TYPE), "edge curve is not a B-spline");
    double a = 0, b = 0;
    require(SmApiEdgeParameterRange(e, a, b) == SM_SUCCESS, "SmApiEdgeParameterRange failed");
    Handle(Geom_BSplineCurve) gc = toOcct(*static_cast<const SmBSplineCurve*>(c));
    SmVertex* vs = e->GetStartVertex();
    SmVertex* ve = e->GetEndVertex();
    require(vs && ve, "edge without vertices");
    TopoDS_Vertex V1 = vertexOf(vs), V2 = vertexOf(ve);
    gp_Pnt pa = gc->Value(a), pb = gc->Value(b);
    gp_Pnt p1 = BRep_Tool::Pnt(V1), p2 = BRep_Tool::Pnt(V2);
    if (vs != ve && pa.Distance(p1) + pb.Distance(p2) > pa.Distance(p2) + pb.Distance(p1)) {
      std::swap(V1, V2);
      std::swap(p1, p2);
    }
    B.UpdateVertex(V1, std::max(BRep_Tool::Tolerance(V1), pa.Distance(p1) + 1e-7));
    B.UpdateVertex(V2, std::max(BRep_Tool::Tolerance(V2), pb.Distance(p2) + 1e-7));
    BRepBuilderAPI_MakeEdge me(gc, V1, V2, a, b);
    require(me.IsDone(), "MakeEdge failed");
    TopoDS_Edge oe = me.Edge();
    emap[e] = oe;
    return oe;
  };

  BRepBuilderAPI_Sewing sew(1e-6);
  int nfaces = 0;
  for (ULONG fi = 0; fi < faces.GetSize(); ++fi) {
    SmFace* f = faces.GetAt(fi);
    SmSurface* srf = f->GetSurface();
    require(srf && srf->IsKindOf(SmBSplineSurface_TYPE), "face surface is not a B-spline");
    Handle(Geom_BSplineSurface) gs = toOcct(*static_cast<const SmBSplineSurface*>(srf));
    SmFaceuse* fu = f->GetUpwardFaceuse();
    require(fu != nullptr, "face has no upward faceuse");
    SmTArray<SmLoopuse*> lus;
    fu->GetLoopuses(lus);
    std::vector<TopoDS_Wire> wires;
    for (ULONG li = 0; li < lus.GetSize(); ++li) {
      SmLoopuse* lu = lus.GetAt(li);
      if (!lu->IsEdgeLoopuse()) continue;  // vertex loops carry no boundary
      SmTArray<SmEdgeuse*> eus;
      lu->GetEdgeuses(eus);
      BRepBuilderAPI_MakeWire mw;
      for (ULONG ei = 0; ei < eus.GetSize(); ++ei) {
        SmEdgeuse* eu = eus.GetAt(ei);
        TopoDS_Edge oe = edgeOf(eu->GetEdge());
        mw.Add(eu->GetOrientation() == SM_OT_OPPOSITE ? TopoDS::Edge(oe.Reversed()) : oe);
      }
      require(mw.IsDone(), "MakeWire failed");
      wires.push_back(mw.Wire());
    }
    require(!wires.empty(), "face has no edge loops");
    BRepBuilderAPI_MakeFace mf(gs, wires[0]);
    for (size_t k = 1; k < wires.size(); ++k) mf.Add(wires[k]);
    require(mf.IsDone(), "MakeFace failed");
    ShapeFix_Face sff(mf.Face());
    sff.SetPrecision(1e-6);
    sff.Perform();  // pcurves by projection, wire order/orientation, seams, poles
    if (debug) {
      const char* kind = srf->IsKindOf(SmPlane_TYPE)      ? "plane"
                         : srf->IsKindOf(SmCylinder_TYPE) ? "cylinder"
                         : srf->IsKindOf(SmCone_TYPE)     ? "cone"
                         : srf->IsKindOf(SmSphere_TYPE)   ? "sphere"
                         : srf->IsKindOf(SmTorus_TYPE)    ? "torus"
                                                          : "bspline";
      int ne = 0;
      for (TopExp_Explorer e(sff.Face(), TopAbs_EDGE); e.More(); e.Next()) ++ne;
      std::cerr << "smlib->occt: face#" << fi << " " << kind << " poles=" << gs->NbUPoles() << "x" << gs->NbVPoles()
                << " periodicU/V=" << gs->IsUPeriodic() << "/" << gs->IsVPeriodic() << " loops=" << wires.size()
                << " edges=" << ne << " sfxFail=" << sff.Status(ShapeExtend_FAIL) << "\n";
    }
    sew.Add(sff.Face());
    nfaces++;
  }
  require(nfaces > 0, "solid has no faces");
  sew.Perform();
  TopoDS_Shape sewed = sew.SewedShape();
  if (debug)
    std::cerr << "smlib->occt: sewing free=" << sew.NbFreeEdges() << " multiple=" << sew.NbMultipleEdges()
              << " degenerated=" << sew.NbDegeneratedShapes() << "\n";
  // One shell per connected solid (a model may be several disjoint bodies).
  // A single self-closed face (torus, full sphere patch) comes back from the
  // sewer as a bare face, not a shell: wrap it.
  std::vector<TopoDS_Shell> shells;
  for (TopExp_Explorer ex(sewed, TopAbs_SHELL); ex.More(); ex.Next()) shells.push_back(TopoDS::Shell(ex.Current()));
  if (shells.empty()) {
    TopoDS_Shell sh;
    B.MakeShell(sh);
    int n = 0;
    for (TopExp_Explorer ex(sewed, TopAbs_FACE); ex.More(); ex.Next(), ++n) B.Add(sh, ex.Current());
    if (n > 0) { sh.Closed(Standard_True); shells.push_back(sh); }
  }
  std::vector<TopoDS_Shape> solids;
  for (const TopoDS_Shell& shell : shells) {
    if (!BRep_Tool::IsClosed(shell)) { solids.push_back(shell); continue; }  // 2D sheet: keep as a shell
    BRepBuilderAPI_MakeSolid ms(shell);
    require(ms.IsDone(), "MakeSolid failed");
    ShapeFix_Solid sfs(ms.Solid());
    sfs.Perform();
    solids.push_back(sfs.Solid());
  }
  require(!solids.empty(), "sewing produced no shell");
  if (solids.size() == 1) return solids[0];
  TopoDS_Compound comp;
  B.MakeCompound(comp);
  for (const auto& s : solids) B.Add(comp, s);
  return comp;
}
