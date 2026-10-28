#pragma once
// The kernel seam. Everything above this line (lexer, parser, evaluator) is
// kernel-agnostic and produces a pure-data Node tree; a Backend turns that tree
// into geometry and writes it out. The built solid stays opaque so that no
// kernel type leaks upward.
#include <memory>
#include <string>
#include <vector>

#include "Node.h"

struct Solid {
  virtual ~Solid() = default;
  // Binary STL, tessellated at the given linear deflection. False on failure.
  virtual bool writeStl(const std::string& path, double linearDeflection) = 0;
  // Exact B-Rep STEP (AP214). False on failure.
  virtual bool writeStep(const std::string& path) = 0;
  // Kernel-side metrics (bbox + volume) as JSON, for kernels with no
  // tessellator. False when unsupported (the default for every backend that
  // tessellates; STL carries the same information).
  virtual bool writeMetrics(const std::string& path) { return false; }
};

struct Backend {
  virtual ~Backend() = default;
  virtual const char* name() const = 0;
  // Build the whole tree. Returns nullptr when the program produced no geometry.
  // Kernel failures surface as std::runtime_error, never as kernel-native
  // exception types.
  virtual std::unique_ptr<Solid> build(const Node& root) = 0;
};

// Names accepted by makeBackend(), in preference order ("occt" first).
std::vector<std::string> availableBackends();
// Throws std::runtime_error for an unknown or not-compiled-in name.
std::unique_ptr<Backend> makeBackend(const std::string& name);
