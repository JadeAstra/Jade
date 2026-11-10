# M9.5's spike: spike.exe (the levels of native code, by hand) and spike.jade (the VM) on the fast
# cores, each printing its own best of five.
#   powershell -ExecutionPolicy Bypass -File bench\spike\spike.ps1 [-Jade build\clang\jade.exe]
param([string]$Jade = 'build\clang\jade.exe')
$here = $PSScriptRoot
$repo = Split-Path (Split-Path $here)
. (Join-Path $repo 'bench\fastcores.ps1')
$mask = Get-FastCoreMask
$exe = if ([IO.Path]::IsPathRooted($Jade)) { $Jade } else { Join-Path $repo $Jade }
"VM ($Jade):"
Invoke-Pinned $exe @((Join-Path $here 'spike.jade')) $mask
"By hand:"
Invoke-Pinned (Join-Path $here 'spike.exe') @() $mask
