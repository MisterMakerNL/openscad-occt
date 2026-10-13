#pragma once
#include <string>

#include <TopoDS_Shape.hxx>

// Tessellate the shape in place (needed before STL/preview).
void tessellate(const TopoDS_Shape& shape, double linearDeflection, double angularDeflection);

// Write binary STL (tessellates with the given linear deflection first).
// Returns false on failure.
bool writeStl(const TopoDS_Shape& shape, const std::string& path, double linearDeflection);

// Write an exact B-Rep STEP file (AP214). Returns false on failure.
bool writeStep(const TopoDS_Shape& shape, const std::string& path);
