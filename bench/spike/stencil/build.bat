@echo off
rem M9.5 step 1: the stencils compiled (src\native\stencils.cpp), the tool built and run on them
rem (tools\stencils), and the proof built on its table; all with the clang-cl that builds ww.exe.
setlocal
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%i"
set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "CLANG=%VSROOT%\VC\Tools\Llvm\x64\bin\clang-cl.exe"
cd /d %~dp0
"%CLANG%" /c /O2 /GS- /Gy /std:c++20 ..\..\..\src\native\stencils.cpp /Fostencils.obj || exit /b 1
"%CLANG%" /O2 /EHsc /std:c++20 ..\..\..\tools\stencils\stencils.cpp /Fe:stencils_tool.exe || exit /b 1
"%~dp0stencils_tool.exe" stencils.obj stencils.h || exit /b 1
"%CLANG%" /O2 /EHsc /std:c++20 /D_CRT_SECURE_NO_WARNINGS proof.cpp /Fe:proof.exe || exit /b 1
