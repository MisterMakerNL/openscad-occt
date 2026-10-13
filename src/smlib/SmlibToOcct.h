#pragma once
#include <TopoDS_Shape.hxx>

class SmBrep;

// Exact transfer of an SMLib solid into OCCT: every surface/curve as NURBS
// (SMLib's analytic types all derive from its B-spline classes), topology from
// SMLib's own shared vertices and edges, pcurves projected by ShapeFix, faces
// sewn into a solid. Throws std::runtime_error on anything it cannot map.
// Known limit: faces containing a surface pole (SmApiCreateSphere) can come out
// unorientable; the backend uses pole-free spheres to avoid this.
TopoDS_Shape smlibToOcct(SmBrep* brep);
