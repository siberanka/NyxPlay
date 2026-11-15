# Uninstalls the virtual gamepad driver (ViGEmBus) using its registered uninstall command.
# Win32_Product is not used: it is slow and triggers a repair check of every installed MSI package.
$timeoutMs = 300000

$uninstallKeys = @(
    "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*",
    "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*"
)
$entry = Get-ItemProperty -Path $uninstallKeys -ErrorAction SilentlyContinue |
    Where-Object { $_.DisplayName -like "ViGEm Bus*" } |
    Select-Object -First 1

if (-not $entry) {
    Write-Host "ViGEm Bus Driver not found"
    exit 0
}

$command = $entry.QuietUninstallString
$quiet = $true
if (-not $command) {
    $command = $entry.UninstallString
    $quiet = $false
}
if (-not $command) {
    Write-Host "ViGEm Bus Driver has no uninstall command"
    exit 1
}

# Split the command into the executable and its arguments
if ($command -match '^\s*"([^"]+)"\s*(.*)$') {
    $file = $Matches[1]
    $arguments = $Matches[2]
} elseif ($command -match '^\s*(\S+)\s*(.*)$') {
    $file = $Matches[1]
    $arguments = $Matches[2]
} else {
    Write-Host "The uninstall command of ViGEm Bus Driver could not be parsed"
    exit 1
}
if (-not $quiet) {
    $arguments = "$arguments /quiet /norestart".Trim()
} elseif ($arguments -notmatch '/norestart') {
    $arguments = "$arguments /norestart".Trim()
}

$process = Start-Process -FilePath $file -ArgumentList $arguments -PassThru
if (-not $process.WaitForExit($timeoutMs)) {
    Write-Host "The ViGEm Bus Driver uninstaller did not finish in time, stopping it."
    Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
    exit 1
}

$exitCode = $process.ExitCode
if ($exitCode -eq 0 -or $exitCode -eq 3010) {
    Write-Host "ViGEm Bus Driver uninstalled successfully"
    exit 0
}
Write-Host "The ViGEm Bus Driver uninstaller failed with exit code $exitCode"
exit $exitCode
