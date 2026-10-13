#pragma once
#include <TopoDS_Shape.hxx>

#include "Node.h"

// Translate a CSG node tree into an OCCT B-Rep solid. Returns a null shape for
// empty/degenerate subtrees (callers must tolerate TopoDS_Shape::IsNull()).
// Throws std::runtime_error if a boolean operation fails.
TopoDS_Shape buildShape(const Node& node);
