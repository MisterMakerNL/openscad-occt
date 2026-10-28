// SolidCore backend: the Node tree -> solidcore-capi C ABI calls. Mirrors the
// smlib backend's semantics for the subset the Rust port covers.
//
// Representation: a 1-based B-rep id in the backend's own ScKernel. Booleans
// consume their operand ids, so ownership is trivial: every consumed id is gone
// and the result carries a fresh one.
//
// Not supported (each fails loud with "solidcore: UNSUP ..."):
//   - 2D primitives, linear_extrude / rotate_extrude (no 2D curve-building API
//     is exported here), mirror, attach;
//   - STL / STEP output: solidcore has no tessellator (sc_mesh returns -1) and
//     no STEP writer; use --metrics for bbox + volume instead;
//   - fillet edges="convex"|"concave": the C ABI has no dihedral classification
//     yet, so the selection falls back to ALL edges (noted, not silent: the
//     metrics JSON records which selector ran).
#include "SolidcoreBackend.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "solidcore_capi.h"

namespace {

[[noreturn]] void fail(const std::string& m) { throw std::runtime_error("solidcore: " + m); }

ScKernel* kern() {
  static ScKernel* k = sc_kernel_new();
  return k;
}

// The fillet selector the last fillet ran with (recorded in the metrics JSON).
std::string g_filletSel = "all";

std::string lastError() {
  const uint8_t* p = sc_last_error(kern());
  return p ? std::string(reinterpret_cast<const char*>(p)) : "";
}

void check(int32_t result, const char* what) {
  if (result != 0) fail(std::string(what) + " failed: " + lastError());
}
void checkId(int64_t id, const char* what) {
  if (id <= 0) fail(std::string(what) + " failed: " + lastError());
}

// OpenSCAD treats non-positive or non-finite dimensions as "nothing".
bool usable(double d) { return std::isfinite(d) && d > 0; }

int64_t build(const Node& node);

int64_t unionOf(const std::vector<NodePtr>& kids) {
  int64_t acc = 0;
  for (const auto& k : kids) {
    int64_t g = build(*k);
    if (g <= 0) continue;
    if (acc <= 0) { acc = g; continue; }
    int64_t r = sc_union(kern(), acc, g);
    checkId(r, "sc_union");
    acc = r;
  }
  return acc;
}

int64_t makeDifference(const std::vector<NodePtr>& kids) {
  if (kids.empty()) return 0;
  int64_t acc = build(*kids[0]);
  if (acc <= 0) return 0;
  for (size_t i = 1; i < kids.size(); ++i) {
    int64_t g = build(*kids[i]);
    if (g <= 0) continue;
    int64_t r = sc_difference(kern(), acc, g);
    checkId(r, "sc_difference");
    acc = r;
    if (sc_volume(kern(), acc) == 0) return 0;  // removed everything
  }
  return acc;
}

int64_t makeIntersection(const std::vector<NodePtr>& kids) {
  if (kids.empty()) return 0;
  int64_t acc = build(*kids[0]);
  if (acc <= 0) return 0;
  for (size_t i = 1; i < kids.size(); ++i) {
    int64_t g = build(*kids[i]);
    if (g <= 0) return 0;
    int64_t r = sc_intersection(kern(), acc, g);
    checkId(r, "sc_intersection");
    acc = r;
    if (sc_volume(kern(), acc) == 0) return 0;
  }
  return acc;
}

// ---- 3D primitives ----
int64_t makeCube(const CubeNode& c) {
  if (!usable(c.x) || !usable(c.y) || !usable(c.z)) return 0;
  double x0 = c.center ? -c.x / 2 : 0, y0 = c.center ? -c.y / 2 : 0, z0 = c.center ? -c.z / 2 : 0;
  int64_t id = sc_box(kern(), x0, y0, z0, c.x, c.y, c.z);
  checkId(id, "sc_box");
  return id;
}

int64_t makeSphere(const SphereNode& s) {
  if (!usable(s.r)) return 0;
  int64_t id = sc_sphere(kern(), 0, 0, 0, s.r);
  checkId(id, "sc_sphere");
  return id;
}

int64_t makeCylinder(const CylinderNode& c) {
  if (!usable(c.h) || !std::isfinite(c.r1) || !std::isfinite(c.r2) || (c.r1 <= 0 && c.r2 <= 0)) return 0;
  double z0 = c.center ? -c.h / 2 : 0;
  int64_t id = c.r1 == c.r2 ? sc_cylinder(kern(), 0, 0, z0, c.r1, c.h)
                            : sc_cone(kern(), 0, 0, z0, c.r1, c.r2, c.h);
  checkId(id, "sc_cylinder/sc_cone");
  return id;
}

// ---- transforms (applied to the built solid) ----
int64_t makeTranslate(const TranslateNode& n) {
  int64_t g = unionOf(n.children);
  if (g <= 0) return g;
  check(sc_translate(kern(), g, n.x, n.y, n.z), "sc_translate");
  return g;
}

int64_t makeRotate(const RotateNode& n) {
  int64_t g = unionOf(n.children);
  if (g <= 0) return g;
  if (n.axisAngle) {
    if (n.ax == 0 && n.ay == 0 && n.az == 0) return g;
    check(sc_rotate(kern(), g, n.angle, n.ax, n.ay, n.az), "sc_rotate");
    return g;
  }
  // Euler: X, then Y, then Z (same order as the other backends).
  if (n.x != 0) check(sc_rotate(kern(), g, n.x, 1, 0, 0), "sc_rotate X");
  if (n.y != 0) check(sc_rotate(kern(), g, n.y, 0, 1, 0), "sc_rotate Y");
  if (n.z != 0) check(sc_rotate(kern(), g, n.z, 0, 0, 1), "sc_rotate Z");
  return g;
}

int64_t makeScale(const ScaleNode& n) {
  int64_t g = unionOf(n.children);
  if (g <= 0) return g;
  check(sc_scale(kern(), g, n.x, n.y, n.z), "sc_scale");
  return g;
}

// ---- B-Rep feature layer ----
int64_t makeFillet(const FilletNode& n) {
  int64_t g = unionOf(n.children);
  if (g <= 0) return g;
  if (n.sel != FilletNode::Sel::All)
    fail("UNSUP: fillet edges=\"" +
         std::string(n.sel == FilletNode::Sel::Convex ? "convex" : "concave") +
         "\" is not available on the solidcore backend (no dihedral classification "
         "in the C ABI); only edges=\"all\" is supported");
  g_filletSel = "all";
  check(sc_fillet_all(kern(), g, n.r), "sc_fillet_all");
  return g;
}

int64_t build(const Node& node) {
  switch (node.kind) {
    case NodeKind::Cube:      return makeCube(static_cast<const CubeNode&>(node));
    case NodeKind::Sphere:    return makeSphere(static_cast<const SphereNode&>(node));
    case NodeKind::Cylinder:  return makeCylinder(static_cast<const CylinderNode&>(node));
    case NodeKind::Translate: return makeTranslate(static_cast<const TranslateNode&>(node));
    case NodeKind::Rotate:    return makeRotate(static_cast<const RotateNode&>(node));
    case NodeKind::Scale:     return makeScale(static_cast<const ScaleNode&>(node));
    case NodeKind::Fillet:    return makeFillet(static_cast<const FilletNode&>(node));
    case NodeKind::Union:
    case NodeKind::Group:        return unionOf(node.children);
    case NodeKind::Difference:   return makeDifference(node.children);
    case NodeKind::Intersection: return makeIntersection(node.children);
    case NodeKind::Square:
    case NodeKind::Circle:
    case NodeKind::Polygon:
      fail("UNSUP: 2D primitives are not available on the solidcore backend (extrude them)");
    case NodeKind::LinearExtrude:
    case NodeKind::RotateExtrude:
      fail("UNSUP: extrusions are not available on the solidcore backend yet");
    case NodeKind::Mirror:
      fail("UNSUP: mirror is not available on the solidcore backend");
    case NodeKind::Attach:
      fail("UNSUP: attach is not available on the solidcore backend");
  }
  fail("unknown node kind");
}

// ---- output ----
struct SolidcoreSolid : Solid {
  int64_t id;
  explicit SolidcoreSolid(int64_t i) : id(i) {}

  bool writeStl(const std::string&, double) override {
    std::cerr << "error: solidcore has no tessellator; no STL (use --metrics for bbox+volume)\n";
    return false;
  }
  bool writeStep(const std::string&) override { return false; }

  bool writeMetrics(const std::string& path) override {
    double bbox[6] = {0, 0, 0, 0, 0, 0};
    bool haveBbox = sc_bbox(kern(), id, bbox) == 0;
    double v = sc_volume(kern(), id);
    std::ofstream out(path);
    if (!out) return false;
    out << "{\"kernel\":\"solidcore\",\"bbox\":[";
    for (int i = 0; i < 6; ++i) out << (i ? "," : "") << bbox[i];
    out << "],\"have_bbox\":" << (haveBbox ? "true" : "false");
    out << ",\"volume\":" << v;
    out << ",\"fillet_sel\":\"" << g_filletSel << "\"";
    out << "}\n";
    return static_cast<bool>(out);
  }
};

struct SolidcoreBackend : Backend {
  const char* name() const override { return "solidcore"; }
  std::unique_ptr<Solid> build(const Node& root) override {
    int64_t id = ::build(root);
    if (id <= 0) return nullptr;
    return std::make_unique<SolidcoreSolid>(id);
  }
};

}  // namespace

std::unique_ptr<Backend> makeSolidcoreBackend() { return std::make_unique<SolidcoreBackend>(); }
