# Every kernel, best of $Runs, in C++ (kernels.exe, from build.bat) and in Jade on each build
# given, all kept on the fast cores (bench\fastcores.ps1): a line per kernel, its time on each,
# for docs/M10-speed.html.
#   powershell -ExecutionPolicy Bypass -File bench\kernels\time.ps1 -Jade build\clang\jade.exe[,other\jade.exe] [-Runs 3] [-Only blur,flood]
param([string[]]$Jade = @('build\clang\jade.exe'), [int]$Runs = 3, [string]$Only = '')
$Jade = $Jade | ForEach-Object { $_ -split ',' }   # one string, from -File
$here = $PSScriptRoot
$repo = Split-Path (Split-Path $here)
. (Join-Path $repo 'bench\fastcores.ps1')
$mask = Get-FastCoreMask
$kernels = @('lineEnds', 'idct', 'huffman', 'gradient', 'radial', 'blur', 'gaussian', 'shrink', 'flood', 'lineIndex', 'lineWorker')
if ($Only) { $kernels = $kernels | Where-Object { ($Only -split ',') -contains $_ } }
$programs = @(@{ name = 'C++'; exe = (Join-Path $here 'kernels.exe'); args = @() })
foreach ($j in $Jade) {
    $exe = if ([IO.Path]::IsPathRooted($j)) { $j } else { Join-Path $repo $j }
    $programs += @{ name = $j; exe = $exe; args = @((Join-Path $here 'kernels.jade')) }
}
"pinned to the fast cores, mask {0:X}; best of $Runs" -f $mask
"{0,-10} {1}" -f 'kernel', (($programs | ForEach-Object { '{0,12}' -f (Split-Path (Split-Path $_.exe) -Leaf) }) -join '')
foreach ($k in $kernels) {
    $best = @{}
    $checks = @{}
    for ($r = 0; $r -lt $Runs; $r++) {
        foreach ($p in $programs) {
            $line = (Invoke-Pinned $p.exe ($p.args + @($k)) $mask | Where-Object { $_ -match "^$k \d+ ms" } | Select-Object -First 1)
            if ($line -notmatch "^$k (\d+) ms \((.*)\)") { throw "$($p.exe) $k gave: $line" }
            $ms = [int]$Matches[1]
            if (-not $best.ContainsKey($p.exe) -or $ms -lt $best[$p.exe]) { $best[$p.exe] = $ms }
            $checks[$p.exe] = $Matches[2]
        }
    }
    $same = if (($checks.Values | Select-Object -Unique).Count -eq 1) { 'same' } else { 'DIFF: ' + (($checks.Values) -join ' | ') }
    "{0,-10} {1}  {2}" -f $k, (($programs | ForEach-Object { '{0,12}' -f $best[$_.exe] }) -join ''), $same
}
