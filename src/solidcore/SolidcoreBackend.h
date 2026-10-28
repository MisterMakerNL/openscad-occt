#pragma once
#include <memory>

#include "Backend.h"

// Third kernel: SolidCore (tangency-labs' Rust port of NVIDIA's SMLib), reached
// through its C ABI in libsolidcore_capi.so (crates/solidcore-capi). Compiled
// in only when CMake is configured with -DOSCAD_WITH_SOLIDCORE=/path/to/solidcore.
// No tessellator is ported, so there is no STL and no STEP: use --metrics for
// kernel-side bbox + volume, which is what tests/compare-kernels.sh compares.
std::unique_ptr<Backend> makeSolidcoreBackend();
