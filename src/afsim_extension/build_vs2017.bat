@echo off
setlocal EnableExtensions
chcp 65001 >nul

if "%~1"=="" goto usage
if "%~2"=="" goto usage

set "SOURCE_DIR=%~dp0."
set "AFSIM_SOURCE_ROOT=%~f1"
set "AFSIM_BUILD_ROOT=%~f2"
set "BUILD_DIR=%~3"
set "BUILD_CONFIG=RelWithDebInfo"

if "%BUILD_DIR%"=="" set "BUILD_DIR=%~dp0build-vs2017"

if not exist "%AFSIM_SOURCE_ROOT%\core\wsf\source" (
  echo [错误] AFSIM 源码目录无效：%AFSIM_SOURCE_ROOT%
  exit /b 2
)
if not exist "%AFSIM_BUILD_ROOT%\lib\%BUILD_CONFIG%\wsf.lib" (
  echo [错误] 未找到 AFSIM %BUILD_CONFIG% 库：%AFSIM_BUILD_ROOT%
  exit /b 2
)

set "CMAKE_EXE=cmake.exe"
where cmake.exe >nul 2>nul
if errorlevel 1 (
  set "CMAKE_EXE=%ProgramFiles%\CMake\bin\cmake.exe"
  if not exist "%ProgramFiles%\CMake\bin\cmake.exe" (
    set "CMAKE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\2017\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
  )
)
if not exist "%CMAKE_EXE%" (
  where "%CMAKE_EXE%" >nul 2>nul
  if errorlevel 1 (
    echo [错误] 未找到 CMake。
    exit /b 3
  )
)

"%CMAKE_EXE%" -S "%SOURCE_DIR%" -B "%BUILD_DIR%" ^
  -G "Visual Studio 15 2017 Win64" ^
  -DAFSIM_SOURCE_ROOT="%AFSIM_SOURCE_ROOT%" ^
  -DAFSIM_BUILD_ROOT="%AFSIM_BUILD_ROOT%" ^
  -DAFSIM_LIBRARY_CONFIG=%BUILD_CONFIG%
if errorlevel 1 exit /b 4

"%CMAKE_EXE%" --build "%BUILD_DIR%" --config %BUILD_CONFIG% --target wsf_afsim_ns3
if errorlevel 1 exit /b 5

set "PLUGIN_DLL=%BUILD_DIR%\%BUILD_CONFIG%\wsf_afsim_ns3.dll"
if not exist "%PLUGIN_DLL%" (
  echo [错误] 构建完成但未找到插件：%PLUGIN_DLL%
  exit /b 6
)

echo [完成] %PLUGIN_DLL%
exit /b 0

:usage
echo 用法：build_vs2017.bat AFSIM源码目录 AFSIM构建目录 [插件构建目录]
echo 示例：build_vs2017.bat E:\afsim_fenbushi\src E:\afsim_fenbushi\src\build E:\afsimns3\output\afsim-extension-build
exit /b 1
