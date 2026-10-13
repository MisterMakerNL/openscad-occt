#include "OcctBackend.h"

#include <Standard_Failure.hxx>
#include <TopoDS_Shape.hxx>

#include <stdexcept>
#include <string>

#include "Export.h"
#include "Kernel.h"

namespace {

// OCCT throws its own hierarchy (not std::exception). Convert at the seam so a
// kernel failure on awkward geometry is a clean error, never a SIGABRT.
template <typename F>
auto guarded(F&& f) -> decltype(f()) {
  try {
    return f();
  } catch (const Standard_Failure& e) {
    throw std::runtime_error(std::string("OCCT: ") + e.GetMessageString());
  }
}

struct OcctSolid : Solid {
  TopoDS_Shape shape;
  explicit OcctSolid(TopoDS_Shape s) : shape(std::move(s)) {}
  bool writeStl(const std::string& path, double linearDeflection) override {
    return guarded([&] { return ::writeStl(shape, path, linearDeflection); });
  }
  bool writeStep(const std::string& path) override {
    return guarded([&] { return ::writeStep(shape, path); });
  }
};

struct OcctBackend : Backend {
  const char* name() const override { return "occt"; }
  std::unique_ptr<Solid> build(const Node& root) override {
    TopoDS_Shape s = guarded([&] { return buildShape(root); });
    if (s.IsNull()) return nullptr;
    return std::make_unique<OcctSolid>(std::move(s));
  }
};

}  // namespace

std::unique_ptr<Backend> makeOcctBackend() { return std::make_unique<OcctBackend>(); }
