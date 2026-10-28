#include "Backend.h"

#include <stdexcept>

#include "occt/OcctBackend.h"
#ifdef OSCAD_WITH_SMLIB
#include "smlib/SmlibBackend.h"
#endif
#ifdef OSCAD_WITH_SOLIDCORE
#include "solidcore/SolidcoreBackend.h"
#endif

std::vector<std::string> availableBackends() {
  std::vector<std::string> v{"occt"};
#ifdef OSCAD_WITH_SMLIB
  v.push_back("smlib");
#endif
#ifdef OSCAD_WITH_SOLIDCORE
  v.push_back("solidcore");
#endif
  return v;
}

std::unique_ptr<Backend> makeBackend(const std::string& name) {
  if (name == "occt") return makeOcctBackend();
#ifdef OSCAD_WITH_SMLIB
  if (name == "smlib") return makeSmlibBackend();
#endif
#ifdef OSCAD_WITH_SOLIDCORE
  if (name == "solidcore") return makeSolidcoreBackend();
#endif
  std::string known;
  for (const auto& n : availableBackends()) known += (known.empty() ? "" : ", ") + n;
  throw std::runtime_error("unknown kernel '" + name + "' (available: " + known + ")");
}
