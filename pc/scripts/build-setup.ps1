<#
Builds the NyxPlay for PC Windows setup (NSIS) from a released portable zip.
No client file is rebuilt or modified: the zip contents are staged and packed as they are.
Usage: build-setup.ps1 -Zip <portable.zip> -Version 2026.11.20.1 [-ExpectedSha256 <hash>] [-OutDir <dir>]
                       [-Work <dir>] [-Makensis <makensis.exe>] [-SetupVersion <label>]
-SetupVersion only changes the version strings of the setup (used to test upgrade and downgrade paths).
#>
param(
    [Parameter(Mandatory = $true)][string]$Zip,
    [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+\.\d+$')][string]$Version,
    [string]$ExpectedSha256 = '',
    [string]$OutDir = '',
    [string]$Work = '',
    [string]$Makensis = '',
    [string]$SetupVersion = ''
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem

$root = Split-Path -Parent $PSScriptRoot
if (-not $OutDir) { $OutDir = Join-Path ([IO.Path]::GetTempPath()) 'nyxplay-setup\out' }
if (-not $Work) { $Work = Join-Path ([IO.Path]::GetTempPath()) 'nyxplay-setup\work' }
if (-not $Makensis) {
    $cmd = Get-Command makensis.exe -ErrorAction SilentlyContinue
    if (-not $cmd) { throw 'makensis.exe not found on PATH: pass -Makensis <path to makensis.exe>.' }
    $Makensis = $cmd.Source
}
$nsi = Join-Path $root 'installer\nyxplay-pc.nsi'
$icon = Join-Path $root 'app\nyxplay.ico'
if (-not $SetupVersion) { $SetupVersion = $Version }

$actual = (Get-FileHash -Algorithm SHA256 -LiteralPath $Zip).Hash.ToLowerInvariant()
if ($ExpectedSha256 -and $actual -ne $ExpectedSha256.ToLowerInvariant()) {
    throw "zip SHA-256 mismatch: $actual"
}
Write-Host "zip sha256 $actual"

$stage = Join-Path $Work "stage-$Version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
$stageFull = [IO.Path]::GetFullPath($stage).TrimEnd('\') + '\'

$za = [IO.Compression.ZipFile]::OpenRead($Zip)
try {
    foreach ($e in $za.Entries) {
        if ($e.FullName.EndsWith('/')) { continue }
        if ($e.FullName -cnotmatch '^[-A-Za-z0-9._ /]+$' -or $e.FullName -match '(^|/)\.\.?(/|$)') {
            throw "unexpected entry name: $($e.FullName)"
        }
        $dest = [IO.Path]::GetFullPath((Join-Path $stage $e.FullName))
        if (-not $dest.StartsWith($stageFull)) { throw "path escape: $($e.FullName)" }
        New-Item -ItemType Directory -Force (Split-Path $dest) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($e, $dest)
    }
} finally { $za.Dispose() }

# Installed copies run in normal mode (settings in the user profile), so the portable marker and
# the portable-only readme stay out of the setup.
foreach ($n in 'portable.dat', 'README.txt') { Remove-Item -Force (Join-Path $stage $n) }

$files = Get-ChildItem -Recurse -File $stage | Sort-Object FullName
$inst = New-Object Text.StringBuilder
$unin = New-Object Text.StringBuilder
$dirs = New-Object 'System.Collections.Generic.SortedSet[string]'
$lastDir = $null
foreach ($f in $files) {
    $rel = $f.FullName.Substring($stageFull.Length).Replace('/', '\')
    $d = Split-Path $rel
    if ($d -ne $lastDir) {
        $out = if ($d) { '$INSTDIR\' + $d } else { '$INSTDIR' }
        [void]$inst.AppendLine("SetOutPath `"$out`"")
        $lastDir = $d
    }
    [void]$inst.AppendLine("File `"$($f.FullName)`"")
    [void]$unin.AppendLine("Delete `"`$INSTDIR\$rel`"")
    for ($p = $d; $p; $p = Split-Path $p) { [void]$dirs.Add($p) }
}
foreach ($d in ($dirs | Sort-Object { $_.Length } -Descending)) {
    [void]$unin.AppendLine("RMDir `"`$INSTDIR\$d`"")
}
$filesNsh = Join-Path $Work "files-$SetupVersion.nsh"
$unfilesNsh = Join-Path $Work "unfiles-$SetupVersion.nsh"
[IO.File]::WriteAllText($filesNsh, $inst.ToString(), (New-Object Text.UTF8Encoding($true)))
[IO.File]::WriteAllText($unfilesNsh, $unin.ToString(), (New-Object Text.UTF8Encoding($true)))

# Licence page text: the shipped LICENSE with CRLF line ends for the rich-edit control.
$lic = Join-Path $Work 'license-crlf.txt'
$text = [IO.File]::ReadAllText((Join-Path $stage 'LICENSE')) -replace "`r?`n", "`r`n"
[IO.File]::WriteAllText($lic, $text, (New-Object Text.UTF8Encoding($false)))

New-Item -ItemType Directory -Force $OutDir | Out-Null
$setup = Join-Path $OutDir "NyxPlayPC-$SetupVersion-windows-x64-setup.exe"
& $Makensis /V2 "/DVERSION=$SetupVersion" "/DOUTFILE=$setup" "/DICON=$icon" `
    "/DLICENSEFILE=$lic" "/DFILES_NSH=$filesNsh" "/DUNFILES_NSH=$unfilesNsh" $nsi
if ($LASTEXITCODE -ne 0) { throw "makensis failed ($LASTEXITCODE)" }
$h = (Get-FileHash -Algorithm SHA256 -LiteralPath $setup).Hash.ToLowerInvariant()
"{0}  {1}  {2} bytes" -f $h, (Split-Path $setup -Leaf), (Get-Item $setup).Length
