@echo off
setlocal

set "SOURCE_DIR=%~dp0."
set "BUILD_DIR=%~dp0build-vs2017"
set "CMAKE_EXE=cmake.exe"
set "CTEST_EXE=ctest.exe"

where cmake.exe >nul 2>nul
if errorlevel 1 (
  set "CMAKE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  set "CTEST_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe"
  if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
    echo CMake was not found on PATH or in Visual Studio 2017 Community.
    exit /b 1
  )
)

"%CMAKE_EXE%" -H"%SOURCE_DIR%" -B"%BUILD_DIR%" ^
  -G "Visual Studio 15 2017 Win64" ^
  -DBUILD_TESTING=ON
if errorlevel 1 exit /b 1

"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release
if errorlevel 1 exit /b 1

pushd "%BUILD_DIR%"
"%CTEST_EXE%" -C Release --output-on-failure
set "TEST_EXIT=%errorlevel%"
popd
exit /b %TEST_EXIT%
