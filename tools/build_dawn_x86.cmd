@echo off
rem Builds third_party\dawn as a 32-bit shared webgpu_dawn.dll (gta_sa.exe is x86).
rem Output: third_party\dawn-build-x86\webgpu_dawn.dll (+ .lib)
setlocal
set ROOT=%~dp0..
set VS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 || exit /b 1
set PATH=%VS%\VC\Tools\Llvm\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%

rem clang-cl like Chromium: MSVC x86 miscompiles Dawn's alignas(8) TypedInteger
rem parameters (callers pass by value, some callees expect a hidden reference).
cmake -S "%ROOT%\third_party\dawn" -B "%ROOT%\third_party\dawn-build-x86" -G Ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl ^
  -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32 ^
  -DDAWN_SUPPORTS_CXX_MODULES=OFF -DCMAKE_CXX_SCAN_FOR_MODULES=OFF ^
  -DDAWN_FETCH_DEPENDENCIES=ON ^
  -DDAWN_BUILD_MONOLITHIC_LIBRARY=SHARED ^
  -DDAWN_ENABLE_D3D12=ON -DDAWN_ENABLE_D3D11=ON ^
  -DDAWN_ENABLE_VULKAN=OFF -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF -DDAWN_ENABLE_NULL=OFF ^
  -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DDAWN_BUILD_BENCHMARKS=OFF -DDAWN_BUILD_PROTOBUF=OFF ^
  -DDAWN_USE_GLFW=OFF ^
  -DTINT_BUILD_TESTS=OFF -DTINT_BUILD_CMD_TOOLS=OFF -DTINT_BUILD_GLSL_VALIDATOR=OFF || exit /b 1

cmake --build "%ROOT%\third_party\dawn-build-x86" --target webgpu_dawn || exit /b 1
