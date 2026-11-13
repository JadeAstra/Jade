# M9.5's spike: spike.exe (the levels of native code, by hand) and spike.ww (the VM) on the fast
# cores, each printing its own best of five.
#   powershell -ExecutionPolicy Bypass -File bench\spike\spike.ps1 [-WinterWolf build\clang\ww.exe]
param([string]$WinterWolf = 'build\clang\ww.exe')
$here = $PSScriptRoot
$repo = Split-Path (Split-Path $here)
. (Join-Path $repo 'bench\fastcores.ps1')
$mask = Get-FastCoreMask
$exe = if ([IO.Path]::IsPathRooted($WinterWolf)) { $WinterWolf } else { Join-Path $repo $WinterWolf }
"VM ($WinterWolf):"
Invoke-Pinned $exe @((Join-Path $here 'spike.ww')) $mask
"By hand:"
Invoke-Pinned (Join-Path $here 'spike.exe') @() $mask
