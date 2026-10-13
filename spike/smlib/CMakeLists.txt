cmake_minimum_required(VERSION 3.16)
project(smlib_spike CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Standalone build of the SMLib kernel + SM_API from a usd-brep checkout.
# No Omniverse repo tooling, no Packman, no OpenUSD. Only the kernel and its
# C-style wrapper, plus our SmBrep -> OCCT -> STEP bridge.
#
#   git clone https://github.com/NVIDIA-Omniverse/usd-brep
#   cmake -S spike/smlib -B /tmp/smlib-build -G Ninja -DUSD_BREP_ROOT=/path/to/usd-brep
#   ninja -C /tmp/smlib-build
set(USD_BREP_ROOT "" CACHE PATH "Path to the usd-brep checkout")
if(NOT USD_BREP_ROOT)
  message(FATAL_ERROR "Set -DUSD_BREP_ROOT=/path/to/usd-brep")
endif()
set(SMLIB_DIR ${USD_BREP_ROOT}/source/SMLib)
set(SMAPI_DIR ${USD_BREP_ROOT}/source/SM_API)

# Mirrors usd-brep premake5.lua (linux, release) + source/SMLib/premake5.lua.
add_compile_definitions(NDEBUG NOMINMAX SM_GRAPHICS_CALLBACKS=1 _GLIBCXX_USE_CXX11_ABI=1)
add_compile_options(-Wno-write-strings -Wno-deprecated -w)

file(GLOB SMLIB_SRC CONFIGURE_DEPENDS ${SMLIB_DIR}/src/*.cpp)
list(FILTER SMLIB_SRC EXCLUDE REGEX "StdAfx\\.cpp$")
add_library(SMLib SHARED ${SMLIB_SRC})
target_include_directories(SMLib PUBLIC ${SMLIB_DIR}/inc)
target_link_libraries(SMLib PUBLIC tbb tbbmalloc)

file(GLOB SMAPI_SRC CONFIGURE_DEPENDS ${SMAPI_DIR}/src/*.cpp)
list(FILTER SMAPI_SRC EXCLUDE REGEX "StdAfx\\.cpp$")
add_library(SM_API SHARED ${SMAPI_SRC})
target_include_directories(SM_API PUBLIC ${SMAPI_DIR}/inc)
target_compile_definitions(SM_API PRIVATE SM_API_EXPORTS)
target_link_libraries(SM_API PUBLIC SMLib)

# Upstream example (plate minus 16 cylinders), built from the checkout as-is.
add_executable(boolean_bounds ${USD_BREP_ROOT}/Examples/SM_API/boolean_bounds.cpp)
target_include_directories(boolean_bounds PRIVATE ${USD_BREP_ROOT}/Examples/SM_API)
target_compile_definitions(boolean_bounds PRIVATE SM_BOOLEAN_BOUNDS_EXAMPLE_STANDALONE)
target_link_libraries(boolean_bounds PRIVATE SM_API)

# STEP bridge: SmBrep -> OCCT TopoDS_Solid -> STEP (AP214), validated with BRepCheck.
find_package(OpenCASCADE REQUIRED)
set(OCCT_LIBS TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim
    TKBO TKBool TKShHealing TKMesh TKXSBase TKDESTEP)
add_executable(smlib_to_occt smlib_to_occt.cc)
target_include_directories(smlib_to_occt PRIVATE ${OpenCASCADE_INCLUDE_DIR})
target_link_libraries(smlib_to_occt PRIVATE SM_API ${OCCT_LIBS})
