cmake_minimum_required(VERSION 3.16)
project(oscad CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(OpenCASCADE REQUIRED)
message(STATUS "OCCT version : ${OpenCASCADE_VERSION}")
message(STATUS "OCCT include : ${OpenCASCADE_INCLUDE_DIR}")

# Minimal toolkit set for Milestone 1. Linking the full ${OpenCASCADE_LIBRARIES}
# would drag in the DRAW test harness (TKDraw/TKXSDRAW/...) from libocct-draw-dev,
# which we neither have nor need. These are imported targets from the OCCT config.
set(OCCT_LIBS
  TKernel TKMath                              # foundation
  TKG2d TKG3d TKGeomBase TKBRep               # modeling-data
  TKGeomAlgo TKTopAlgo TKPrim                 # primitives + topo algos
  TKBO TKBool TKFillet TKOffset TKShHealing   # booleans / healing
  TKMesh                                      # tessellation (BRepMesh)
  TKXSBase TKDESTEP TKDESTL)                  # data exchange: STEP + STL

# Toolchain smoke test -- verifies OCCT build/link/export independent of the interpreter.
add_executable(smoke_occt tests/smoke_occt.cc)
target_include_directories(smoke_occt PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(smoke_occt PRIVATE ${OCCT_LIBS})

# Frames/datums mechanic proof -- exact face-to-face mate from a topology query (no lang).
add_executable(frame_smoke tests/frame_smoke.cc)
target_include_directories(frame_smoke PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(frame_smoke PRIVATE ${OCCT_LIBS})

# Exact B-Rep validity checker -- reads a STEP, runs BRepCheck (validity != watertight STL).
add_executable(step_check tests/step_check.cc)
target_include_directories(step_check PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(step_check PRIVATE ${OCCT_LIBS})

# The interpreter.
# Kernel-agnostic front-end + backend seam. Backends live in src/<kernel>/.
add_executable(oscad
  src/main.cc
  src/Lexer.cc
  src/Parser.cc
  src/Evaluator.cc
  src/Backend.cc
  src/occt/OcctBackend.cc
  src/occt/Kernel.cc
  src/occt/Export.cc)
target_include_directories(oscad PRIVATE src ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(oscad PRIVATE ${OCCT_LIBS})

# Optional second kernel: SMLib from a NVIDIA usd-brep checkout. Selected at
# run time with `oscad --kernel smlib`. See cmake/SMLib.cmake and DESIGN.md 9.1.
set(OSCAD_WITH_SMLIB "" CACHE PATH "Path to a usd-brep checkout; enables the smlib backend")
if(OSCAD_WITH_SMLIB)
  include(cmake/SMLib.cmake)
  target_sources(oscad PRIVATE src/smlib/SmlibBackend.cc src/smlib/SmlibToOcct.cc)
  set_source_files_properties(src/smlib/SmlibBackend.cc src/smlib/SmlibToOcct.cc
    PROPERTIES COMPILE_OPTIONS "-w")  # SMLib headers are noisy under -Wall
  target_compile_definitions(oscad PRIVATE OSCAD_WITH_SMLIB)
  target_link_libraries(oscad PRIVATE SM_API)
  message(STATUS "smlib backend: ON (${OSCAD_WITH_SMLIB})")
else()
  message(STATUS "smlib backend: OFF (set -DOSCAD_WITH_SMLIB=/path/to/usd-brep)")
endif()
