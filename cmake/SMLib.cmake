# Builds the SMLib kernel and its SM_API wrapper straight from a NVIDIA usd-brep
# checkout (https://github.com/NVIDIA-Omniverse/usd-brep, Apache-2.0). No
# Omniverse repo tooling, no Packman, no OpenUSD: only source/SMLib and
# source/SM_API, linked against TBB. Mirrors usd-brep's premake5.lua (linux,
# release) + source/SMLib/premake5.lua.
#
# Expects OSCAD_WITH_SMLIB to point at the checkout; defines targets SMLib and SM_API.
set(SMLIB_DIR ${OSCAD_WITH_SMLIB}/source/SMLib)
set(SMAPI_DIR ${OSCAD_WITH_SMLIB}/source/SM_API)
if(NOT EXISTS ${SMLIB_DIR}/inc/SmBrep.h)
  message(FATAL_ERROR "OSCAD_WITH_SMLIB=${OSCAD_WITH_SMLIB} is not a usd-brep checkout")
endif()

file(GLOB SMLIB_SRC CONFIGURE_DEPENDS ${SMLIB_DIR}/src/*.cpp)
list(FILTER SMLIB_SRC EXCLUDE REGEX "StdAfx\\.cpp$")
add_library(SMLib SHARED ${SMLIB_SRC})
target_include_directories(SMLib PUBLIC ${SMLIB_DIR}/inc)
# PUBLIC: these affect header layout, so every consumer must see the same set.
target_compile_definitions(SMLib PUBLIC NDEBUG NOMINMAX SM_GRAPHICS_CALLBACKS=1 _GLIBCXX_USE_CXX11_ABI=1)
target_compile_options(SMLib PRIVATE -w)
target_link_libraries(SMLib PUBLIC tbb tbbmalloc)

file(GLOB SMAPI_SRC CONFIGURE_DEPENDS ${SMAPI_DIR}/src/*.cpp)
list(FILTER SMAPI_SRC EXCLUDE REGEX "StdAfx\\.cpp$")
add_library(SM_API SHARED ${SMAPI_SRC})
target_include_directories(SM_API PUBLIC ${SMAPI_DIR}/inc)
target_compile_definitions(SM_API PRIVATE SM_API_EXPORTS)
target_compile_options(SM_API PRIVATE -w)
target_link_libraries(SM_API PUBLIC SMLib)
