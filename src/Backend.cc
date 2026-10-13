#include "Backend.h"

#include <stdexcept>

#include "occt/OcctBackend.h"
#ifdef OSCAD_WITH_SMLIB
#include "smlib/SmlibBackend.h"
#endif

std::vector<std::string> availableBackends() {
  std::vector<std::string> v{"occt"};
#ifdef OSCAD_WITH_SMLIB
  v.push_back("smlib");
#endif
  return v;
}

std::unique_ptr<Backend> makeBackend(const std::string& name) {
  if (name == "occt") return makeOcctBackend();
#ifdef OSCAD_WITH_SMLIB
  if (name == "smlib") return makeSmlibBackend();
#endif
  std::string known;
  for (const auto& n : availableBackends()) known += (known.empty() ? "" : ", ") + n;
  throw std::runtime_error("unknown kernel '" + name + "' (available: " + known + ")");
}
