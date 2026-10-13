#pragma once
#include <memory>

#include "Backend.h"

// The default backend: OpenCASCADE (OCCT). Wraps Kernel.cc + Export.cc.
std::unique_ptr<Backend> makeOcctBackend();
