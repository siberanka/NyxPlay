<#
.SYNOPSIS
    Guards the single shared copy of moonlight-common-c.

.DESCRIPTION
    The library lives only in common/moonlight-common-c. The script fails when
      - a second copy of the library (a Limelight.h outside of the shared directory) is tracked,
      - .gitmodules registers a moonlight-common-c checkout outside of common/,
      - the shared copy lacks one of its required files, carries a .github directory,
      - the ENet or nanors submodule is not at the commit recorded in VENDORED.md.

.EXAMPLE
    powershell -NoProfile -File common/check-single-copy.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$shared = 'common/moonlight-common-c'

$errors = New-Object System.Collections.Generic.List[string]

function Invoke-Git {
    $out = & git -C $root @args
    if ($LASTEXITCODE -ne 0) { throw "git $args failed" }
    return $out
}

# 1. Stray copies of the library: the public header is the marker
$headers = @(Invoke-Git ls-files --cached --others --exclude-standard -- '*Limelight.h') | Where-Object { $_ }
foreach ($h in $headers) {
    $dir = $h -replace '/src/Limelight\.h$', ''
    if ($dir -eq $shared) { continue }
    $errors.Add("second copy of moonlight-common-c: $dir (only $shared is allowed)")
}

# 2. Submodules registered outside of the shared directory
$gitmodules = Join-Path $root '.gitmodules'
$modulePaths = @()
if (Test-Path $gitmodules) {
    $modulePaths = @(& git -C $root config -f .gitmodules --get-regexp '^submodule\..*\.path$' | ForEach-Object { ($_ -split ' ', 2)[1] })
}
foreach ($p in $modulePaths) {
    if ($p -like '*moonlight-common-c*' -and $p -ne $shared -and -not $p.StartsWith($shared + '/')) {
        $errors.Add("submodule outside of ${shared}: $p")
    }
}
foreach ($required in @("$shared/enet", "$shared/nanors")) {
    if ($modulePaths -notcontains $required) { $errors.Add(".gitmodules does not register $required") }
}

# 3. Required content of the shared copy
$sharedDir = Join-Path $root $shared
foreach ($file in @('LICENSE.txt', 'VENDORED.md', 'CMakeLists.txt', 'src/Limelight.h', 'src/NyxExtensions.h', 'src/lc_atomic.h')) {
    if (-not (Test-Path (Join-Path $sharedDir $file))) { $errors.Add("missing $shared/$file") }
}
if (Test-Path (Join-Path $sharedDir '.github')) { $errors.Add("$shared/.github must not exist") }
if (Test-Path (Join-Path $sharedDir '.gitmodules')) { $errors.Add("$shared/.gitmodules must not exist, the root .gitmodules registers the submodules") }

# 4. Submodule commits equal the ones recorded in VENDORED.md
$vendored = Join-Path $sharedDir 'VENDORED.md'
if (Test-Path $vendored) {
    $text = Get-Content -Raw $vendored
    foreach ($name in @('enet', 'nanors')) {
        $m = [regex]::Match($text, "\(submodule ``$name``\)[^\r\n]*commit ``([0-9a-f]{7,40})``")
        if (-not $m.Success) { $errors.Add("VENDORED.md does not record the $name commit"); continue }
        $recorded = $m.Groups[1].Value
        $line = @(Invoke-Git ls-files -s -- "$shared/$name") | Select-Object -First 1
        if (-not $line) { $errors.Add("$shared/$name is not tracked as a submodule"); continue }
        $actual = ($line -split '\s+')[1]
        if (-not $actual.StartsWith($recorded)) { $errors.Add("$shared/$name is at $actual, VENDORED.md records $recorded") }
    }
}

if ($errors.Count -gt 0) {
    foreach ($e in $errors) { Write-Host "error: $e" }
    Write-Host "check-single-copy: FAILED ($($errors.Count) error(s))"
    exit 1
}
Write-Host "check-single-copy: OK"
exit 0
