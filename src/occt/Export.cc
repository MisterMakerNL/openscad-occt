#include "Export.h"

#include <BRepMesh_IncrementalMesh.hxx>
#include <STEPControl_Writer.hxx>
#include <StlAPI_Writer.hxx>

void tessellate(const TopoDS_Shape& shape, double linearDeflection, double angularDeflection) {
  if (shape.IsNull()) return;
  BRepMesh_IncrementalMesh mesher(shape, linearDeflection, Standard_False, angularDeflection,
                                  Standard_True);
  mesher.Perform();
}

bool writeStl(const TopoDS_Shape& shape, const std::string& path, double linearDeflection) {
  if (shape.IsNull()) return false;
  tessellate(shape, linearDeflection, 0.5);
  StlAPI_Writer writer;
  writer.ASCIIMode() = Standard_False;  // binary
  return writer.Write(shape, path.c_str()) == Standard_True;
}

bool writeStep(const TopoDS_Shape& shape, const std::string& path) {
  if (shape.IsNull()) return false;
  STEPControl_Writer writer;
  if (writer.Transfer(shape, STEPControl_AsIs) != IFSelect_RetDone) return false;
  return writer.Write(path.c_str()) == IFSelect_RetDone;
}
