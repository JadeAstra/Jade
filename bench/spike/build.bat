@echo off
rem spike.cpp built with clang-cl, as spike.exe beside it (not kept in git): the flags of ww.exe's Release build.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d %~dp0
"%VSROOT%\VC\Tools\Llvm\x64\bin\clang-cl.exe" /O2 /EHsc /std:c++20 /D_CRT_SECURE_NO_WARNINGS spike.cpp /Fe:spike.exe || exit /b 1
