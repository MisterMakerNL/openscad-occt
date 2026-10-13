#pragma once
#include <memory>

#include "Backend.h"

// Second kernel: SMLib (NVIDIA usd-brep, Apache-2.0). Compiled in only when
// CMake is configured with -DOSCAD_WITH_SMLIB=/path/to/usd-brep.
// STL comes from SMLib's own tessellator; STEP goes through an exact
// SmBrep -> OCCT bridge (SmlibToOcct.cc) because SMLib has no STEP writer.
std::unique_ptr<Backend> makeSmlibBackend();
