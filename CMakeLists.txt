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
  TKXSBase TKSTEP TKSTL)                   # data exchange: STEP + STL

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

# Optional third kernel: SolidCore (Rust port of SMLib) through its C ABI
# cdylib, built by the solidcore workspace itself (cargo build -p solidcore-capi
# --release). Selected at run time with `oscad --kernel solidcore`.
set(OSCAD_WITH_SOLIDCORE "" CACHE PATH "Path to a solidcore checkout; enables the solidcore backend")
if(OSCAD_WITH_SOLIDCORE)
  find_library(SOLIDCORE_CAPI_LIB solidcore_capi
    PATHS "${OSCAD_WITH_SOLIDCORE}/bin/release" "${OSCAD_WITH_SOLIDCORE}/target/release"
    NO_DEFAULT_PATH)
  if(NOT SOLIDCORE_CAPI_LIB)
    message(FATAL_ERROR "solidcore_capi library not found under ${OSCAD_WITH_SOLIDCORE}/bin/release (or target/release); run 'cargo build -p solidcore-capi --release' in the solidcore checkout first")
  endif()
  add_library(solidcore_capi SHARED IMPORTED)
  set_target_properties(solidcore_capi PROPERTIES IMPORTED_LOCATION "${SOLIDCORE_CAPI_LIB}")
  target_sources(oscad PRIVATE src/solidcore/SolidcoreBackend.cc)
  target_compile_definitions(oscad PRIVATE OSCAD_WITH_SOLIDCORE)
  target_link_libraries(oscad PRIVATE solidcore_capi)
  # The cdylib is loaded at run time from the cargo target dir.
  set_target_properties(oscad PROPERTIES BUILD_RPATH "${OSCAD_WITH_SOLIDCORE}/target/release")
  message(STATUS "solidcore backend: ON (${SOLIDCORE_CAPI_LIB})")
else()
  message(STATUS "solidcore backend: OFF (set -DOSCAD_WITH_SOLIDCORE=/path/to/solidcore)")
endif()
