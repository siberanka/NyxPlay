# Installs the virtual gamepad driver (ViGEmBus).
# -Silent: no user interface and no restart prompt (used by silent installations).
param(
    [switch]$Silent
)

# Longest time the driver installer may run before it is stopped
$timeoutMs = 120000

# Check if a compatible version of ViGEmBus is already installed (1.17 or later)
try {
    $vigemBusPath = "$env:SystemRoot\System32\drivers\ViGEmBus.sys"
    # Compare as [version]; string comparison would rank "1.9.0.0" above "1.17"
    $fileVersion = [System.Version](Get-Item $vigemBusPath -ErrorAction Stop).VersionInfo.FileVersion

    if ($fileVersion -ge [System.Version]"1.17") {
        Write-Host "The installed version is 1.17 or later, no update needed. Exiting."
        exit 0
    }
}
catch {
    Write-Host "ViGEmBus driver not found or inaccessible, proceeding with installation."
}

# Install Virtual Gamepad
$scriptPath = Split-Path -Parent $MyInvocation.MyCommand.Path
$installerPath = Join-Path $scriptPath "vigembus_installer.exe"
if (-not (Test-Path -LiteralPath $installerPath)) {
    Write-Host "The ViGEmBus installer is missing: $installerPath"
    exit 2
}

if ($Silent) {
    $installerArgs = @("/quiet", "/norestart")
} else {
    $installerArgs = @("/passive", "/norestart")
}

$process = Start-Process -FilePath $installerPath -ArgumentList $installerArgs -PassThru
if (-not $process.WaitForExit($timeoutMs)) {
    Write-Host "The ViGEmBus installer did not finish within $($timeoutMs / 1000) seconds, stopping it."
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    exit 1
}

$exitCode = $process.ExitCode
if ($exitCode -eq 3010) {
    Write-Host "ViGEmBus was installed. A restart is required before the virtual gamepad can be used."
    exit 0
}
if ($exitCode -ne 0) {
    Write-Host "The ViGEmBus installer failed with exit code $exitCode."
    exit $exitCode
}
Write-Host "ViGEmBus was installed."
exit 0
