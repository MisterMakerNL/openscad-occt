// Spike: SMLib SmBrep -> OCCT TopoDS_Solid -> STEP.
//
// Strategy (deliberately simple): every SMLib surface/curve is ultimately a
// NURBS (SmPlane/SmCylinder/... derive from SmBSplineSurface; SmLine/SmCircle
// derive from SmBSplineCurve), so we pull the NURBS data out uniformly, build
// faces from surface + 3D edge wires, let ShapeFix project pcurves, sew, and
// make a solid. Analytic-type mapping (plane -> gp_Pln etc.) is a refinement.

#include <SmApiBrep.h>
#include <SmApiFillets.h>
#include <SmApiGeneral.h>
#include <SmApiPrimitives.h>
#include <SmApiQueries.h>
#include <SmBSplineCurve.h>
#include <SmBSplineSurface.h>
#include <SmBrep.h>
#include <SmEdge.h>
#include <SmEdgeuse.h>
#include <SmFace.h>
#include <SmFaceuse.h>
#include <SmLoopuse.h>
#include <SmPlane.h>
#include <SmCylinder.h>
#include <SmCone.h>
#include <SmSphere.h>
#include <SmTorus.h>
#include <SmTArray.h>
#include <SmVector3d.h>
#include <SmVertex.h>

#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRep_Builder.hxx>
#include <TopoDS_Vertex.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeSolid.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Sewing.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepCheck_Wire.hxx>
#include <BRepCheck_Result.hxx>
#include <BRepCheck_ListIteratorOfListOfStatus.hxx>
#include <BRepLib.hxx>
#include <ShapeFix_Shape.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <GCPnts_AbscissaPoint.hxx>
#include <TopoDS_Edge.hxx>
#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <Geom_BSplineCurve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeFix_Solid.hxx>
#include <Standard_Failure.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TColStd_Array1OfReal.hxx>
#include <TColStd_Array2OfReal.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TColgp_Array2OfPnt.hxx>
#include <TopExp_Explorer.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shell.hxx>
#include <TopoDS_Solid.hxx>
#include <TopoDS_Wire.hxx>

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void Check(SmApiStatus st, const char* what) {
  if (st != SM_SUCCESS) throw std::runtime_error(std::string(what) + " returned " + std::to_string(st));
}
void Require(bool ok, const char* what) {
  if (!ok) throw std::runtime_error(what);
}

// Collapse a flat knot vector into OCCT's (unique knots, multiplicities) form.
// If the flat vector is short by one at each end (ncp+deg-1 style), pad the
// end multiplicities -- that is the clamped-equivalent representation.
struct Knots {
  TColStd_Array1OfReal k;
  TColStd_Array1OfInteger m;
  Knots(const std::vector<double>& uk, const std::vector<int>& um)
      : k(1, (int)uk.size()), m(1, (int)um.size()) {
    for (size_t i = 0; i < uk.size(); ++i) { k((int)i + 1) = uk[i]; m((int)i + 1) = um[i]; }
  }
};

Knots collapse(const std::vector<double>& flat, int ncp, int deg, const char* tag) {
  std::vector<double> uk;
  std::vector<int> um;
  for (double v : flat) {
    if (!uk.empty() && std::abs(v - uk.back()) < 1e-12) um.back()++;
    else { uk.push_back(v); um.push_back(1); }
  }
  int sum = 0;
  for (int x : um) sum += x;
  if (sum == ncp + deg - 1) { um.front()++; um.back()++; sum += 2; }
  if (sum != ncp + deg + 1)
    throw std::runtime_error(std::string(tag) + ": knot count " + std::to_string(sum) + " != ncp+deg+1 = " + std::to_string(ncp + deg + 1));
  Require(um.front() == deg + 1 && um.back() == deg + 1, "unclamped knot vector (not handled in spike)");
  return Knots(uk, um);
}

Handle(Geom_BSplineCurve) toOcct(const SmBSplineCurve& c) {
  const int deg = (int)c.GetDegree();
  const int ncp = (int)c.GetNumberControlPoints();
  SmTArray<double> flat;
  Require(c.GetKnotsAll(flat) == SM_SUCCESS, "GetKnotsAll");
  std::vector<double> fk;
  for (ULONG i = 0; i < flat.GetSize(); ++i) fk.push_back(flat[i]);
  Knots kn = collapse(fk, ncp, deg, "curve");
  const bool rational = c.IsRational() == TRUE;
  TColgp_Array1OfPnt poles(1, ncp);
  TColStd_Array1OfReal w(1, ncp);
  for (int i = 0; i < ncp; ++i) {
    SmPoint3d p;
    double wi = 1.0;
    Require(c.GetControlPoint(SM_CP_EUCLIDIAN_RATIONAL, (ULONG)i, p, wi) == SM_SUCCESS, "curve GetControlPoint");
    poles(i + 1) = gp_Pnt(p[0], p[1], p[2]);
    w(i + 1) = wi;
  }
  if (rational) return new Geom_BSplineCurve(poles, w, kn.k, kn.m, deg);
  return new Geom_BSplineCurve(poles, kn.k, kn.m, deg);
}

Handle(Geom_BSplineSurface) toOcct(const SmBSplineSurface& s) {
  const int du = (int)s.GetDegree(SM_SP_U), dv = (int)s.GetDegree(SM_SP_V);
  const int nu = (int)s.GetNumberControlPoints(SM_SP_U), nv = (int)s.GetNumberControlPoints(SM_SP_V);
  auto knotsOf = [&](SmSurfParamType dir, int ncp, int deg, const char* tag) {
    SmTArray<double> uk;
    SmTArray<ULONG> um;
    Require(s.GetKnots(dir, uk, &um) == SM_SUCCESS, "surface GetKnots");
    std::vector<double> flat;
    for (ULONG i = 0; i < uk.GetSize(); ++i)
      for (ULONG r = 0; r < um[i]; ++r) flat.push_back(uk[i]);
    return collapse(flat, ncp, deg, tag);
  };
  Knots ku = knotsOf(SM_SP_U, nu, du, "surface U");
  Knots kv = knotsOf(SM_SP_V, nv, dv, "surface V");
  const bool rational = s.IsRational() == TRUE;
  TColgp_Array2OfPnt poles(1, nu, 1, nv);
  TColStd_Array2OfReal w(1, nu, 1, nv);
  for (int i = 0; i < nu; ++i)
    for (int j = 0; j < nv; ++j) {
      SmPoint3d p;
      double wi = 1.0;
      Require(s.GetControlPoint(SM_CP_EUCLIDIAN_RATIONAL, (ULONG)i, (ULONG)j, p, wi) == SM_SUCCESS, "surface GetControlPoint");
      poles(i + 1, j + 1) = gp_Pnt(p[0], p[1], p[2]);
      w(i + 1, j + 1) = wi;
    }
  if (rational) return new Geom_BSplineSurface(poles, w, ku.k, kv.k, ku.m, kv.m, du, dv);
  return new Geom_BSplineSurface(poles, ku.k, kv.k, ku.m, kv.m, du, dv);
}


// Print BRepCheck statuses of every invalid sub-shape (integer codes; see BRepCheck_Status.hxx).
void dumpInvalid(const TopoDS_Shape& shape) {
  BRepCheck_Analyzer ana(shape);
  if (ana.IsValid()) return;
  const TopAbs_ShapeEnum kinds[] = {TopAbs_SOLID, TopAbs_SHELL, TopAbs_FACE, TopAbs_WIRE, TopAbs_EDGE, TopAbs_VERTEX};
  const char* names[] = {"solid", "shell", "face", "wire", "edge", "vertex"};
  for (int k = 0; k < 6; ++k) {
    int idx = 0, bad = 0;
    std::map<int, int> hist;
    for (TopExp_Explorer ex(shape, kinds[k]); ex.More(); ex.Next(), ++idx) {
      const TopoDS_Shape& s = ex.Current();
      if (ana.IsValid(s)) continue;
      bad++;
      if (kinds[k] == TopAbs_FACE) {
        Handle(Geom_BSplineSurface) bs = Handle(Geom_BSplineSurface)::DownCast(BRep_Tool::Surface(TopoDS::Face(s)));
        int ne = 0;
        for (TopExp_Explorer ee(s, TopAbs_EDGE); ee.More(); ee.Next()) ++ne;
        std::cout << "    invalid face: poles=" << (bs.IsNull() ? -1 : bs->NbUPoles()) << "x" << (bs.IsNull() ? -1 : bs->NbVPoles()) << " edges=" << ne << "\n";
      }
      Handle(BRepCheck_Result) r = ana.Result(s);
      if (r.IsNull()) continue;
      for (r->InitContextIterator(); r->MoreShapeInContext(); r->NextShapeInContext())
        for (BRepCheck_ListIteratorOfListOfStatus it(r->StatusOnShape()); it.More(); it.Next())
          if (it.Value() != BRepCheck_NoError) hist[(int)it.Value()]++;
      for (BRepCheck_ListIteratorOfListOfStatus it(r->Status()); it.More(); it.Next())
        if (it.Value() != BRepCheck_NoError) hist[(int)it.Value()]++;
    }
    if (bad) {
      std::cout << "    invalid " << names[k] << "s=" << bad << " statuses:";
      for (auto& kv : hist) std::cout << " " << kv.first << "x" << kv.second;
      std::cout << "\n";
    }
  }
}

struct Stats {
  int faces = 0, loops = 0, edgeuses = 0, seamEdges = 0;
};

TopoDS_Shape brepToOcct(SmBrep* brep, Stats& st) {
  SmTArray<SmFace*> faces;
  Check(SmApiGetFaces(brep, faces), "SmApiGetFaces");
  BRep_Builder B;
  std::map<SmVertex*, TopoDS_Vertex> vmap;
  std::map<SmEdge*, TopoDS_Edge> emap;
  double maxGap = 0;
  int swaps = 0;

  auto vertexOf = [&](SmVertex* v) {
    auto it = vmap.find(v);
    if (it != vmap.end()) return it->second;
    SmPoint3d p;
    Check(SmApiVertexGetPoint(v, p), "SmApiVertexGetPoint");
    TopoDS_Vertex tv = BRepBuilderAPI_MakeVertex(gp_Pnt(p[0], p[1], p[2]));
    vmap[v] = tv;
    return tv;
  };
  // One OCCT edge per SMLib edge, bounded by SMLib's shared vertices. Vertex
  // tolerance grows to cover any gap between the curve end and the vertex.
  auto edgeOf = [&](SmEdge* e) {
    auto it = emap.find(e);
    if (it != emap.end()) return it->second;
    SmCurve* c = e->GetCurve();
    Require(c && c->IsKindOf(SmBSplineCurve_TYPE), "edge curve is not a SmBSplineCurve");
    double a = 0, b = 0;
    Check(SmApiEdgeParameterRange(e, a, b), "SmApiEdgeParameterRange");
    Handle(Geom_BSplineCurve) gc = toOcct(*static_cast<const SmBSplineCurve*>(c));
    SmVertex* vs = e->GetStartVertex();
    SmVertex* ve = e->GetEndVertex();
    Require(vs && ve, "edge without vertices");
    TopoDS_Vertex V1 = vertexOf(vs), V2 = vertexOf(ve);
    gp_Pnt pa = gc->Value(a), pb = gc->Value(b);
    gp_Pnt p1 = BRep_Tool::Pnt(V1), p2 = BRep_Tool::Pnt(V2);
    if (vs != ve && pa.Distance(p1) + pb.Distance(p2) > pa.Distance(p2) + pb.Distance(p1)) {
      std::swap(V1, V2);
      std::swap(p1, p2);
      swaps++;
    }
    double g1 = pa.Distance(p1), g2 = pb.Distance(p2);
    maxGap = std::max(maxGap, std::max(g1, g2));
    B.UpdateVertex(V1, std::max(BRep_Tool::Tolerance(V1), g1 + 1e-7));
    B.UpdateVertex(V2, std::max(BRep_Tool::Tolerance(V2), g2 + 1e-7));
    BRepBuilderAPI_MakeEdge me(gc, V1, V2, a, b);
    Require(me.IsDone(), "MakeEdge failed");
    TopoDS_Edge oe = me.Edge();
    emap[e] = oe;
    return oe;
  };

  BRepBuilderAPI_Sewing sew(1e-6);
  for (ULONG fi = 0; fi < faces.GetSize(); ++fi) {
    SmFace* f = faces.GetAt(fi);
    SmSurface* srf = f->GetSurface();
    Require(srf && srf->IsKindOf(SmBSplineSurface_TYPE), "face surface is not a SmBSplineSurface");
    Handle(Geom_BSplineSurface) gs = toOcct(*static_cast<const SmBSplineSurface*>(srf));

    SmFaceuse* fu = f->GetUpwardFaceuse();
    Require(fu != nullptr, "face has no upward faceuse");
    SmTArray<SmLoopuse*> lus;
    fu->GetLoopuses(lus);
    std::vector<TopoDS_Wire> wires;
    int vloops = 0;
    for (ULONG li = 0; li < lus.GetSize(); ++li) {
      SmLoopuse* lu = lus.GetAt(li);
      if (lu->IsVertexLoopuse()) { vloops++; continue; }
      if (!lu->IsEdgeLoopuse()) continue;
      SmTArray<SmEdgeuse*> eus;
      lu->GetEdgeuses(eus);
      BRepBuilderAPI_MakeWire mw;
      for (ULONG ei = 0; ei < eus.GetSize(); ++ei) {
        SmEdgeuse* eu = eus.GetAt(ei);
        SmEdge* e = eu->GetEdge();
        if (e->IsSeam()) st.seamEdges++;
        TopoDS_Edge oe = edgeOf(e);
        mw.Add(eu->GetOrientation() == SM_OT_OPPOSITE ? TopoDS::Edge(oe.Reversed()) : oe);
        st.edgeuses++;
      }
      Require(mw.IsDone(), "MakeWire failed");
      wires.push_back(mw.Wire());
      st.loops++;
    }
    Require(!wires.empty(), "face has no edge loops");
    BRepBuilderAPI_MakeFace mf(gs, wires[0]);
    for (size_t k = 1; k < wires.size(); ++k) mf.Add(wires[k]);
    Require(mf.IsDone(), "MakeFace failed");
    ShapeFix_Face sff(mf.Face());
    sff.SetPrecision(1e-6);
    sff.Perform();  // adds pcurves by projection, fixes wire order/orientation, missing seams, poles
    TopoDS_Face of = sff.Face();
    // --- per-face diagnostics (only print when something looks off) ---
    {
      const char* kind = srf->IsKindOf(SmPlane_TYPE)      ? "plane"
                         : srf->IsKindOf(SmCylinder_TYPE) ? "cylinder"
                         : srf->IsKindOf(SmCone_TYPE)     ? "cone"
                         : srf->IsKindOf(SmSphere_TYPE)   ? "sphere"
                         : srf->IsKindOf(SmTorus_TYPE)    ? "torus"
                                                          : "bspline";
      int nw = 0, badWires = 0, ne = 0;
      for (TopExp_Explorer w(of, TopAbs_WIRE); w.More(); w.Next(), ++nw) {
        BRepCheck_Wire cw(TopoDS::Wire(w.Current()));
        if (cw.Closed() != BRepCheck_NoError || cw.Orientation(of) != BRepCheck_NoError) badWires++;
      }
      for (TopExp_Explorer e(of, TopAbs_EDGE); e.More(); e.Next()) ++ne;
      if (badWires || vloops || sff.Status(ShapeExtend_FAIL))
        std::cout << "  face#" << fi << " " << kind << " poles=" << gs->NbUPoles() << "x" << gs->NbVPoles()
                  << " loops=" << wires.size() << " vertexLoops=" << vloops << " wires=" << nw
                  << " badWires=" << badWires << " edges=" << ne << " sfxFail=" << sff.Status(ShapeExtend_FAIL) << "\n";
    }
    sew.Add(of);
    st.faces++;
  }
  std::cout << "topology: smlib vertices=" << vmap.size() << " edges=" << emap.size()
            << " max_vertex_gap=" << maxGap << " endpoint_swaps=" << swaps << "\n";
  sew.Perform();
  TopoDS_Shape sewed = sew.SewedShape();
  std::cout << "sewing: free=" << sew.NbFreeEdges() << " multiple=" << sew.NbMultipleEdges()
            << " contiguous=" << sew.NbContigousEdges() << " degenerated=" << sew.NbDegeneratedShapes() << "\n";
  BRepTools::Write(sewed, "sewed.brep");
  TopExp_Explorer ex(sewed, TopAbs_SHELL);
  Require(ex.More(), "sewing produced no shell");
  TopoDS_Shell shell = TopoDS::Shell(ex.Current());
  ex.Next();
  Require(!ex.More(), "sewing produced more than one shell");
  BRepBuilderAPI_MakeSolid ms(shell);
  Require(ms.IsDone(), "MakeSolid failed");
  ShapeFix_Solid sfs(ms.Solid());
  sfs.Perform();
  TopoDS_Shape solid = sfs.Solid();
  std::cout << "valid after ShapeFix_Solid: " << BRepCheck_Analyzer(solid).IsValid() << "\n";
  dumpInvalid(solid);
  BRepLib::SameParameter(solid, 1e-6, Standard_True);
  std::cout << "valid after SameParameter : " << BRepCheck_Analyzer(solid).IsValid() << "\n";
  ShapeFix_Shape sfx(solid);
  sfx.Perform();
  solid = sfx.Shape();
  std::cout << "valid after ShapeFix_Shape: " << BRepCheck_Analyzer(solid).IsValid() << "\n";
  dumpInvalid(solid);
  return solid;
}

}  // namespace

// Fixture 1: the SM_API example. 12x12x1.5 plate, 4x4 through-holes r=0.75.
// Has an analytic volume, so it checks the bridge against ground truth.
SmBrep* fixturePlate(double& expected) {
  const int rows = 4;
  const double sp = 3.0, size = rows * sp, h = 1.5, r = 0.75;
  SmBrep* plate = nullptr;
  Check(SmApiCreateBox(SmVector3d(0, 0, 0), size, size, h, plate), "SmApiCreateBox");
  for (int i = 0; i < rows; ++i)
    for (int j = 0; j < rows; ++j) {
      SmBrep* tool = nullptr;
      Check(SmApiCreateCylinder(SmVector3d((j + 0.5) * sp, (i + 0.5) * sp, -1.0), r, h + 2.0, tool), "SmApiCreateCylinder");
      SmBrep* res = nullptr;
      Check(SmApiBooleanDifference(plate, tool, res), "SmApiBooleanDifference");
      Require(res == plate, "boolean result must alias primary");
    }
  expected = size * size * h - rows * rows * std::acos(-1.0) * r * r * h;
  return plate;
}

// Fixture 2: the things that usually break a NURBS->B-Rep bridge.
// box U cylinder (seam + non-planar/planar junction), fillet every edge
// (blend surfaces), then subtract a sphere whose south pole lies inside the
// box (degenerate pole edge) and which also bites the cylinder.
bool gNoPole = false;
SmBrep* fixtureHard(double& expected) {
  SmBrep* body = nullptr;
  Check(SmApiCreateBox(SmVector3d(0, 0, 0), 20, 20, 10, body), "SmApiCreateBox");
  SmBrep* post = nullptr;
  Check(SmApiCreateCylinder(SmVector3d(10, 10, 10), 5, 10, post), "SmApiCreateCylinder");
  SmBrep* res = nullptr;
  Check(SmApiBooleanUnion(body, post, res), "SmApiBooleanUnion");
  Require(res == body, "union result must alias primary");
  SmApiStatus fst = SmApiCircularFillet(body, 1.0);
  std::cout << "fillet(all edges, r=1): " << (fst == SM_SUCCESS ? "ok" : ("status " + std::to_string(fst))) << "\n";
  SmBrep* ball = nullptr;
  if (gNoPole) Check(SmApiCreateSphereNoPole(SmVector3d(16, 16, 10), 6, ball), "SmApiCreateSphereNoPole");
  else Check(SmApiCreateSphere(SmVector3d(16, 16, 10), 6, ball), "SmApiCreateSphere");
  Check(SmApiBooleanDifference(body, ball, res), "SmApiBooleanDifference(sphere)");
  Require(res == body, "difference result must alias primary");
  expected = -1;  // no closed form; compare the two kernels against each other
  return body;
}

int main(int argc, char** argv) {
  const std::string out = argc > 1 ? argv[1] : "plate.step";
  const std::string which = argc > 2 ? argv[2] : "plate";
  gNoPole = argc > 3 && std::string(argv[3]) == "nopole";
  try {
    SmApiCreateContext();
    double expected = 0;
    SmBrep* plate = which == "hard" ? fixtureHard(expected) : fixturePlate(expected);
    SmBoolean manifold = FALSE;
    Check(SmApiBrepIsManifoldSolid(plate, manifold), "SmApiBrepIsManifoldSolid");
    double smVol = 0;
    Check(SmApiBrepComputeVolume(plate, 1e-7, smVol), "SmApiBrepComputeVolume");
    if (expected < 0) expected = smVol;
    std::cout << "smlib manifold_solid=" << (manifold ? "YES" : "NO") << "\n";

    Stats st;
    TopoDS_Shape solid = brepToOcct(plate, st);
    const bool valid = BRepCheck_Analyzer(solid).IsValid();
    GProp_GProps gp, gpTight;
    BRepGProp::VolumeProperties(solid, gp);
    BRepGProp::VolumeProperties(solid, gpTight, 1e-9);  // adaptive integration, eps=1e-9
    int occtFaces = 0;
    for (TopExp_Explorer e(solid, TopAbs_FACE); e.More(); e.Next()) occtFaces++;

    STEPControl_Writer w;
    Require(w.Transfer(solid, STEPControl_AsIs) == IFSelect_RetDone, "STEP transfer");
    Require(w.Write(out.c_str()) == IFSelect_RetDone, "STEP write");

    std::cout << "smlib: faces=" << st.faces << " loops=" << st.loops << " edgeuses=" << st.edgeuses
              << " seam_edgeuses=" << st.seamEdges << " volume=" << smVol << "\n"
              << "occt : faces=" << occtFaces << " brep_valid=" << (valid ? "YES" : "NO")
              << " volume=" << gp.Mass() << " volume_eps1e-9=" << gpTight.Mass() << "\n"
              << "expected volume=" << expected << " rel_err(occt)=" << std::abs(gp.Mass() - expected) / expected << " rel_err(occt,eps)=" << std::abs(gpTight.Mass() - expected) / expected << "\n"
              << "wrote " << out << "\n";
    delete plate;
    return valid ? 0 : 2;
  } catch (const Standard_Failure& e) {
    std::cerr << "error: OCCT: " << e.GetMessageString() << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
