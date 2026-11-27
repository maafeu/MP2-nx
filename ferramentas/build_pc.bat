@echo off
rem The PC test build of a port (host-pc): build_pc.bat <game folder> [target]
rem   build\<name>-pc\<name>_pc.exe <folder with default.xbe> [--docked]
rem MaxPayne2-nx: Visual Studio's Clang when it has one, else LLVM's (C:\Program Files\LLVM); the Vulkan headers and
rem import library from third_party\vulkan when there is no LunarG SDK (third_party\README.md); JOBS limits the
rem compile jobs (the generated code takes 1-2 GB per job).
setlocal
set "GAME=%~f1"
if "%GAME%"=="" (echo usage: build_pc.bat ^<game folder^> & exit /b 1)
set "TARGET=%~2"
for %%i in ("%~dp0..") do set "KIT=%%~fi"
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VSDIR=%%i"
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "LLVMBIN=%VSDIR%\VC\Tools\Llvm\x64\bin"
if not exist "%LLVMBIN%\clang.exe" set "LLVMBIN=%ProgramFiles%\LLVM\bin"
set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%LLVMBIN%;%PATH%"
set "VKARG="
if "%VULKAN_SDK%"=="" if exist "%KIT%\third_party\vulkan\Lib\vulkan-1.lib" set VKARG="-DVULKAN_SDK_DIR=%KIT%\third_party\vulkan"
if "%JOBS%"=="" set "JOBS=12"
for %%i in ("%GAME%") do set "NAME=%%~nxi"
set "OUT=%KIT%\build\%NAME%-pc"
if not exist "%OUT%\build.ninja" (
  cmake -S "%KIT%\host-pc" -B "%OUT%" -G Ninja -DPORT_GAME_DIR="%GAME%" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_POLICY_VERSION_MINIMUM=3.5 %VKARG% || exit /b 1
)
if "%TARGET%"=="" (cmake --build "%OUT%" -j %JOBS%) else (cmake --build "%OUT%" -j %JOBS% --target %TARGET%)
