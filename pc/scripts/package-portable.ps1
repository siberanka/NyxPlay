<#
.SYNOPSIS
  Builds the NyxPlay portable zip from a clean staging directory.

.DESCRIPTION
  Takes an already built NyxPlay.exe (release) and AntiHooking.dll, runs windeployqt
  without the QML debugger plugins, adds the bundled runtime libraries, the licence
  files and an empty portable.dat marker, verifies that no user data or debug
  components ended up in the tree and writes NyxPlayPC-<version>-windows-<arch>-portable.zip.

  The staging directory is always created from scratch and must never be a directory
  in which NyxPlay has been started, because a started portable copy writes its
  settings (client identity key, paired hosts) next to the executable.

.PARAMETER BuildDir
  qmake build directory that holds app\release\NyxPlay.exe and AntiHooking\release\AntiHooking.dll.
  Default: <repository>\pc-build

.PARAMETER QtBin
  Qt bin directory that provides windeployqt.exe.
  Default: $env:QTDIR\bin when QTDIR is set (required otherwise).

.PARAMETER OutDir
  Working and output directory. Staging goes to <OutDir>\stage, the zip to <OutDir>.
  Default: <repository>\dist

.PARAMETER Arch
  Package architecture label and library folder (x64 or arm64). Default: x64

.PARAMETER VcRuntimeDir
  Optional directory with the Visual C++ runtime DLLs (vcruntime140*.dll, msvcp140*.dll)
  to include. Without it the package requires the Visual C++ 2015-2022 x64 redistributable.

.EXAMPLE
  powershell -NoProfile -ExecutionPolicy Bypass -File pc\scripts\package-portable.ps1

.NOTES
  Exit code 0 on success, 1 when any verification fails. discord-rpc.dll is not packaged
  because the client is built without Discord integration. Code signing is not part of
  this script.
#>
[CmdletBinding()]
param(
    [string]$BuildDir = '',
    [string]$QtBin = '',
    [string]$OutDir = '',
    [string]$Arch = 'x64',
    [string]$VcRuntimeDir = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2

$pcRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $pcRoot
$version = (Get-Content -LiteralPath (Join-Path $pcRoot 'app\version.txt') -Raw).Trim()
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot 'pc-build' }
if (-not $OutDir) { $OutDir = Join-Path $repoRoot 'dist' }
if (-not $QtBin) {
    if (-not $env:QTDIR) { throw 'Pass -QtBin (Qt bin directory with windeployqt.exe) or set QTDIR.' }
    $QtBin = Join-Path $env:QTDIR 'bin'
}
$stage = Join-Path $OutDir 'stage'
$zipPath = Join-Path $OutDir "NyxPlayPC-$version-windows-$Arch-portable.zip"
$exeSrc = Join-Path $BuildDir 'app\release\NyxPlay.exe'
$ahSrc = Join-Path $BuildDir 'AntiHooking\release\AntiHooking.dll'
$libDir = Join-Path $pcRoot "libs\windows\lib\$Arch"
$errors = New-Object System.Collections.Generic.List[string]

function Fail([string]$msg) { $script:errors.Add($msg); Write-Host "FAIL: $msg" -ForegroundColor Red }
function Step([string]$msg) { Write-Host "== $msg" }

foreach ($p in @($exeSrc, $ahSrc, (Join-Path $QtBin 'windeployqt.exe'), $libDir)) {
    if (-not (Test-Path -LiteralPath $p)) { throw "Missing input: $p" }
}

# The exe must carry the version recorded in version.txt
$pv = (Get-Item -LiteralPath $exeSrc).VersionInfo.ProductVersion
if ($pv -ne $version) { throw "NyxPlay.exe ProductVersion '$pv' does not match version.txt '$version'. Rebuild first." }

Step "Preparing clean staging directory $stage"
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$outFull = (Resolve-Path -LiteralPath $OutDir).Path
if (Test-Path -LiteralPath $stage) {
    $stageFull = (Resolve-Path -LiteralPath $stage).Path
    if (-not $stageFull.StartsWith($outFull, [StringComparison]::OrdinalIgnoreCase) -or (Split-Path -Leaf $stageFull) -ne 'stage') {
        throw "Refusing to delete unexpected path $stageFull"
    }
    Remove-Item -LiteralPath $stageFull -Recurse -Force
}
New-Item -ItemType Directory -Path $stage | Out-Null
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
$stageFull = (Resolve-Path -LiteralPath $stage).Path

Step 'Copying binaries'
Copy-Item -LiteralPath $exeSrc -Destination $stage
Copy-Item -LiteralPath $ahSrc -Destination $stage
Get-ChildItem -LiteralPath $libDir -Filter '*.dll' |
    Where-Object { $_.Name -ne 'discord-rpc.dll' } |
    Copy-Item -Destination $stage
Copy-Item -LiteralPath (Join-Path $pcRoot 'app\SDL_GameControllerDB\gamecontrollerdb.txt') -Destination $stage
if ($VcRuntimeDir) {
    Get-ChildItem -LiteralPath $VcRuntimeDir -Filter '*.dll' | Copy-Item -Destination $stage
}

Step 'Deploying Qt'
$deployArgs = @(
    '--dir', $stage, '--release', '--qmldir', (Join-Path $pcRoot 'app\gui'),
    '--no-opengl-sw', '--no-compiler-runtime', '--no-sql',
    '--no-system-d3d-compiler', '--no-system-dxc-compiler',
    '--skip-plugin-types', 'qmltooling,generic', '--no-ffmpeg', '--force-openssl',
    '--no-quickcontrols2fusion', '--no-quickcontrols2imagine', '--no-quickcontrols2universal',
    '--no-quickcontrols2fusionstyleimpl', '--no-quickcontrols2imaginestyleimpl',
    '--no-quickcontrols2universalstyleimpl', '--no-quickcontrols2windowsstyleimpl',
    '--no-quickcontrols2fluentwinui3styleimpl',
    (Join-Path $stage 'NyxPlay.exe')
)
$env:PATH = "$QtBin;$env:PATH"
& (Join-Path $QtBin 'windeployqt.exe') @deployArgs | Out-Null
if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with exit code $LASTEXITCODE" }

# Unused Quick Controls styles and debugger plugins
foreach ($d in 'qml\QtQuick\Controls\Fusion', 'qml\QtQuick\Controls\Imagine', 'qml\QtQuick\Controls\Universal',
               'qml\QtQuick\Controls\Windows', 'qml\QtQuick\Controls\FluentWinUI3', 'qml\QtQuick\NativeStyle',
               'qmltooling') {
    $p = Join-Path $stage $d
    if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Recurse -Force }
}
Remove-Item -LiteralPath (Join-Path $stage 'icuuc.dll') -Force -ErrorAction SilentlyContinue

Step 'Adding markers and licence files'
New-Item -ItemType Directory -Path (Join-Path $stage 'boxart') | Out-Null
[IO.File]::WriteAllBytes((Join-Path $stage 'portable.dat'), [byte[]]@())
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $stage 'LICENSE')
Copy-Item -LiteralPath (Join-Path $repoRoot 'CREDITS.md') -Destination (Join-Path $stage 'CREDITS.md')

$notices = New-Object System.Text.StringBuilder
[void]$notices.AppendLine('NyxPlay third-party notices')
[void]$notices.AppendLine('===========================')
[void]$notices.AppendLine('')
[void]$notices.AppendLine('NyxPlay is distributed under the GNU General Public License v3.0 (see LICENSE). It is based on')
[void]$notices.AppendLine('Moonlight (Copyright Cameron Gutman and the Moonlight Game Streaming Project, GPL-3.0). See CREDITS.md')
[void]$notices.AppendLine('for the full list of upstream projects and authors. The complete corresponding source code is')
[void]$notices.AppendLine('available at https://github.com/siberanka/NyxPlay')
[void]$notices.AppendLine('')
[void]$notices.AppendLine('This package contains the following third-party components. Components under the LGPL are used')
[void]$notices.AppendLine('as separate dynamic libraries (DLL files); you may replace them with your own compatible builds')
[void]$notices.AppendLine('of the same library. The corresponding source of the LGPL libraries is available from the')
[void]$notices.AppendLine('projects listed below and, on request, from the NyxPlay repository.')
[void]$notices.AppendLine('')
$components = @(
    @('Qt 6.12.0 (Core, Gui, Network, Qml, Quick, Quick Controls 2, Svg and others)', 'LGPL-3.0 (LGPL-3.0.txt, Qt-NOTICE-GPL-3.0.txt)', 'https://www.qt.io, source: https://download.qt.io'),
    @('FFmpeg (avcodec 63.1.102, avformat, avutil, swscale)', 'LGPL-2.1 or later (LGPL-2.1-FFmpeg.txt)', 'https://ffmpeg.org'),
    @('libplacebo v7.387.0', 'LGPL-2.1 or later (LGPL-2.1-libplacebo.txt)', 'https://code.videolan.org/videolan/libplacebo'),
    @('dav1d 1.5.4', 'BSD-2-Clause (BSD-2-Clause-dav1d.txt)', 'https://code.videolan.org/videolan/dav1d'),
    @('Opus', 'BSD-3-Clause', 'https://opus-codec.org'),
    @('SDL2, SDL3, SDL2_ttf', 'zlib', 'https://libsdl.org'),
    @('OpenSSL (libssl, libcrypto)', 'Apache-2.0', 'https://www.openssl.org'),
    @('SDL_GameControllerDB (gamecontrollerdb.txt)', 'zlib', 'https://github.com/mdqinc/SDL_GameControllerDB'),
    @('moonlight-common-c', 'GPL-3.0', 'https://github.com/moonlight-stream/moonlight-common-c'),
    @('QmDnsEngine', 'MIT', 'https://github.com/nitroshare/qmdnsengine'),
    @('h264bitstream', 'LGPL-2.1', 'https://github.com/aizvorski/h264bitstream'),
    @('Microsoft Detours 4.0.1 (AntiHooking.dll)', 'MIT (MIT-Microsoft-Detours.txt)', 'https://github.com/microsoft/Detours')
)
foreach ($c in $components) {
    [void]$notices.AppendLine("* $($c[0])")
    [void]$notices.AppendLine("    License: $($c[1])")
    [void]$notices.AppendLine("    Project: $($c[2])")
}
[void]$notices.AppendLine('')
[void]$notices.AppendLine('The licence texts follow; they are also available as separate files in the licenses folder.')
foreach ($f in @(
        @('QmDnsEngine', (Join-Path $pcRoot 'qmdnsengine\qmdnsengine\LICENSE.txt')),
        @('h264bitstream', (Join-Path $pcRoot 'h264bitstream\LICENSE')),
        @('SDL_GameControllerDB', (Join-Path $pcRoot 'app\SDL_GameControllerDB\LICENSE')))) {
    if (Test-Path -LiteralPath $f[1]) {
        [void]$notices.AppendLine('')
        [void]$notices.AppendLine("---- $($f[0]) ----")
        [void]$notices.AppendLine((Get-Content -LiteralPath $f[1] -Raw))
    } else { Fail "Licence file missing: $($f[1])" }
}
$licStage = Join-Path $stage 'licenses'
New-Item -ItemType Directory -Path $licStage | Out-Null
$licSrc = Join-Path $pcRoot 'licenses'
if (-not (Test-Path -LiteralPath $licSrc)) { Fail "Licence directory missing: $licSrc" }
else {
    foreach ($f in @(Get-ChildItem -LiteralPath $licSrc -File | Sort-Object Name)) {
        Copy-Item -LiteralPath $f.FullName -Destination $licStage
        [void]$notices.AppendLine('')
        [void]$notices.AppendLine("---- $($f.BaseName) ----")
        [void]$notices.AppendLine((Get-Content -LiteralPath $f.FullName -Raw))
    }
}
$mcc = Join-Path $pcRoot 'moonlight-common-c\moonlight-common-c'
foreach ($f in @(
        @('enet', (Join-Path $mcc 'enet\LICENSE')),
        @('nanors', (Join-Path $mcc 'nanors\LICENSE')),
        @('moonlight-common-c', (Join-Path $mcc 'LICENSE.txt')))) {
    if (Test-Path -LiteralPath $f[1]) {
        [void]$notices.AppendLine('')
        [void]$notices.AppendLine("---- $($f[0]) ----")
        [void]$notices.AppendLine((Get-Content -LiteralPath $f[1] -Raw))
        Copy-Item -LiteralPath $f[1] -Destination (Join-Path $licStage ("$($f[0])-LICENSE.txt"))
    } else { Fail "Licence file missing: $($f[1])" }
}
[IO.File]::WriteAllText((Join-Path $stage 'THIRD-PARTY-NOTICES.txt'), $notices.ToString(), (New-Object Text.UTF8Encoding($false)))

Step 'Verifying staging directory'
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File)
$badExt = '.ini', '.log', '.dmp', '.pdb', '.key', '.pem', '.crt', '.cer'
foreach ($b in @($files | Where-Object { $badExt -contains $_.Extension.ToLower() -or $_.Name -like 'qmldbg_*' -or $_.Name -eq 'discord-rpc.dll' })) {
    Fail "Forbidden file in package: $($b.FullName.Substring($stageFull.Length + 1))"
}
if (Test-Path -LiteralPath (Join-Path $stage 'qmltooling')) { Fail 'qmltooling directory present' }
if (Test-Path -LiteralPath (Join-Path $stage 'NyxPlay') -PathType Container) { Fail 'NyxPlay settings directory present' }
if (@(Get-ChildItem -LiteralPath (Join-Path $stage 'boxart') -Force).Count -ne 0) { Fail 'boxart is not empty' }
if ((Get-Item -LiteralPath (Join-Path $stage 'portable.dat')).Length -ne 0) { Fail 'portable.dat is not empty' }
foreach ($r in 'NyxPlay.exe', 'AntiHooking.dll', 'LICENSE', 'CREDITS.md', 'THIRD-PARTY-NOTICES.txt', 'licenses\LGPL-3.0.txt', 'licenses\LGPL-2.1.txt', 'gamecontrollerdb.txt',
               'Qt6Core.dll', 'SDL2.dll', 'avcodec-63.dll', 'platforms\qwindows.dll') {
    if (-not (Test-Path -LiteralPath (Join-Path $stage $r))) { Fail "Required file missing: $r" }
}
$textExt = '.txt', '.md', '.json', '.qml', '.conf', '.dat', '.xml'
foreach ($f in @($files | Where-Object { $textExt -contains $_.Extension.ToLower() -or $_.Name -eq 'LICENSE' })) {
    if (Select-String -LiteralPath $f.FullName -Pattern 'PRIVATE KEY' -SimpleMatch -Quiet) { Fail "Private key material in $($f.Name)" }
}
if ($errors.Count -gt 0) { Write-Host "$($errors.Count) verification error(s); no zip written." -ForegroundColor Red; exit 1 }

Step "Writing $zipPath"
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::Open($zipPath, 'Create')
try {
    foreach ($f in $files) {
        $entry = $f.FullName.Substring($stageFull.Length + 1).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip, $f.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
    }
    # Empty directories are not created from files; add the boxart folder explicitly
    [void]$zip.CreateEntry('boxart/')
} finally { $zip.Dispose() }

$hash = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLower()
Write-Host "OK: $zipPath"
Write-Host "SHA256: $hash"
Write-Host "Files: $($files.Count)"
