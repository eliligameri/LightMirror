@echo off
setlocal
where cmake >nul 2>nul || (
  echo CMake was not found in PATH.
  exit /b 1
)

cmake -S . -B build -A x64 || exit /b 1
cmake --build build --config Release --parallel || exit /b 1

echo.
echo Built: build\Release\LightMirror.exe
endlocal
