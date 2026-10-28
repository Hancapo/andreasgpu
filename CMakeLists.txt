cmake_minimum_required(VERSION 3.24)
project(andreasgpu LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "gta_sa.exe is 32-bit: configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Dawn, built for x86 by tools/build_dawn_x86.cmd as one shared webgpu_dawn.dll.
set(DAWN_SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/dawn)
set(DAWN_BUILD_DIR ${CMAKE_SOURCE_DIR}/third_party/dawn-build-x86)
add_library(dawn_webgpu SHARED IMPORTED)
set_target_properties(dawn_webgpu PROPERTIES
  IMPORTED_LOCATION ${DAWN_BUILD_DIR}/webgpu_dawn.dll
  IMPORTED_IMPLIB ${DAWN_BUILD_DIR}/src/dawn/native/webgpu_dawn.lib
  INTERFACE_INCLUDE_DIRECTORIES "${DAWN_SOURCE_DIR}/include;${DAWN_BUILD_DIR}/gen/include"
  INTERFACE_COMPILE_DEFINITIONS WGPU_SHARED_LIBRARY)

# Dawn loads its shader compilers from its own folder (FXC for D3D11, DXC for D3D12);
# Windows has no x86 DXC, so ship the Windows SDK redistributables next to it.
set(WINSDK_D3D_REDIST "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x86")
set(DAWN_RUNTIME_FILES
  ${DAWN_BUILD_DIR}/webgpu_dawn.dll
  ${DAWN_BUILD_DIR}/webgpu_dawn.pdb
  ${WINSDK_D3D_REDIST}/d3dcompiler_47.dll
  ${WINSDK_D3D_REDIST}/dxcompiler.dll
  ${WINSDK_D3D_REDIST}/dxil.dll)

add_subdirectory(shim)

add_executable(agpu_smoke tests/smoke.cpp)
target_link_libraries(agpu_smoke PRIVATE dbghelp)

add_executable(agpu_dawn_smoke tests/dawn_smoke.cpp)
target_link_libraries(agpu_dawn_smoke PRIVATE dawn_webgpu dbghelp)
add_custom_command(TARGET agpu_dawn_smoke POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${DAWN_RUNTIME_FILES} $<TARGET_FILE_DIR:agpu_dawn_smoke>)
