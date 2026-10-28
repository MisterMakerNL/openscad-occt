// oscad -- a minimal SCAD interpreter on a B-Rep kernel (OCCT by default).
//   oscad input.scad [-o out.stl] [--step out.step] [--deflection 0.1] [--kernel occt]
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "Backend.h"
#include "Evaluator.h"
#include "Lexer.h"
#include "Parser.h"

namespace {

std::string readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open input file: " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string replaceExt(const std::string& path, const std::string& ext) {
  size_t slash = path.find_last_of("/\\");
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return path + ext;
  return path.substr(0, dot) + ext;
}

void usage() {
  std::string kernels;
  for (const auto& k : availableBackends()) kernels += (kernels.empty() ? "" : "|") + k;
  std::cerr << "usage: oscad input.scad [-o out.stl] [--step out.step] "
               "[--deflection 0.1] [--kernel " << kernels << "]\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string input, stlOut, stepOut, metricsOut, kernel = "occt";
  double deflection = 0.1;

  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&](const char* opt) -> std::string {
      if (i + 1 >= argc) { std::cerr << opt << " needs an argument\n"; std::exit(2); }
      return argv[++i];
    };
    if (a == "-o" || a == "--out") stlOut = next(a.c_str());
    else if (a == "--step") stepOut = next(a.c_str());
  else if (a == "--metrics") metricsOut = next(a.c_str());
    else if (a == "--deflection") deflection = std::stod(next(a.c_str()));
    else if (a == "--kernel") kernel = next(a.c_str());
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (!a.empty() && a[0] == '-') { std::cerr << "unknown option: " << a << "\n"; usage(); return 2; }
    else input = a;
  }

  if (input.empty()) { usage(); return 2; }
  if (stlOut.empty() && stepOut.empty() && metricsOut.empty()) stlOut = replaceExt(input, ".stl");

  try {
    std::unique_ptr<Backend> backend = makeBackend(kernel);
    std::string src = readFile(input);
    auto tokens = tokenize(src);
    auto program = parse(tokens);
    NodePtr root = evaluate(program);
    std::unique_ptr<Solid> solid = backend->build(*root);

    if (!solid) {
      std::cerr << "error: program produced no geometry\n";
      return 1;
    }

    bool ok = true;
    if (!stlOut.empty()) {
      if (solid->writeStl(stlOut, deflection)) std::cout << "wrote " << stlOut << "\n";
      else { std::cerr << "error: STL write failed\n"; ok = false; }
    }
    if (!stepOut.empty()) {
      if (solid->writeStep(stepOut)) std::cout << "wrote " << stepOut << "\n";
      else { std::cerr << "error: STEP write failed\n"; ok = false; }
    }
    if (!metricsOut.empty()) {
      if (solid->writeMetrics(metricsOut)) std::cout << "wrote " << metricsOut << "\n";
      else { std::cerr << "error: metrics write not supported by this kernel\n"; ok = false; }
    }
    return ok ? 0 : 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  } catch (...) {
    std::cerr << "error: unrecognized failure\n";
    return 1;
  }
}
