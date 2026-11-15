# Adds or removes the NyxPlay Host directories in the system Path.
#
# The value is read and written through the .NET registry API (Unicode, unexpanded, original value
# type kept), so entries with any character, %VARIABLES% and long values survive unchanged. Only the
# NyxPlay Host entries are touched. The previous value is saved before every change.
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [ValidateSet('add', 'remove')]
    [string]$Action,

    # Directory of the NyxPlay Host installation (default: parent of the script directory)
    [string]$InstallDir = "",

    # Registry location of the Path value (overridable for tests)
    [ValidateSet('HKLM', 'HKCU')]
    [string]$Hive = 'HKLM',
    [string]$KeyPath = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment',
    [string]$ValueName = 'Path',

    # Where the previous value is stored (admin-only HKLM key)
    [string]$BackupKeyPath = 'SOFTWARE\NyxPlay Host',
    [string]$BackupValueName = 'PathBackup',

    [switch]$NoBroadcast
)

$ErrorActionPreference = 'Stop'

function ConvertTo-EntryKey([string]$Entry) {
    # Comparison key: surrounding spaces, quotes and trailing backslashes do not matter, case does not matter
    return $Entry.Trim().Trim('"').TrimEnd('\').ToLowerInvariant()
}

function Send-EnvironmentChange {
    try {
        Add-Type -Namespace NyxPlay -Name NativeMethods -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true, CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr SendMessageTimeout(System.IntPtr hWnd, uint Msg, System.UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out System.UIntPtr lpdwResult);
'@
        $result = [System.UIntPtr]::Zero
        # HWND_BROADCAST, WM_SETTINGCHANGE, SMTO_ABORTIFHUNG
        [void][NyxPlay.NativeMethods]::SendMessageTimeout([System.IntPtr]0xffff, 0x1A, [System.UIntPtr]::Zero, 'Environment', 2, 5000, [ref]$result)
    } catch {
        Write-Host "Could not notify running programs about the Path change: $($_.Exception.Message)"
    }
}

try {
    if ([string]::IsNullOrEmpty($InstallDir)) {
        $InstallDir = Split-Path -Parent $PSScriptRoot
    }
    $InstallDir = $InstallDir.TrimEnd('\')
    $managed = @($InstallDir, ($InstallDir + '\tools'))
    Write-Host "NyxPlay Host root directory: $InstallDir"

    $baseKey = if ($Hive -eq 'HKLM') { [Microsoft.Win32.RegistryHive]::LocalMachine } else { [Microsoft.Win32.RegistryHive]::CurrentUser }
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($baseKey, [Microsoft.Win32.RegistryView]::Registry64)
    $key = $base.OpenSubKey($KeyPath, $true)
    if ($null -eq $key) {
        Write-Host "Failed to open the Path registry key."
        exit 1
    }

    $current = $key.GetValue($ValueName, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    if ($null -eq $current -or ([string]$current).Trim().Length -eq 0) {
        Write-Host "The current Path could not be read or is empty, it is left unchanged."
        exit 1
    }
    $current = [string]$current
    $kind = $key.GetValueKind($ValueName)
    if ($kind -ne [Microsoft.Win32.RegistryValueKind]::String -and $kind -ne [Microsoft.Win32.RegistryValueKind]::ExpandString) {
        $kind = [Microsoft.Win32.RegistryValueKind]::ExpandString
    }

    $entries = New-Object System.Collections.Generic.List[string]
    foreach ($entry in $current.Split(';')) {
        $entries.Add($entry)
    }
    $managedKeys = @($managed | ForEach-Object { ConvertTo-EntryKey $_ })

    $changed = $false
    if ($Action -eq 'add') {
        foreach ($dir in $managed) {
            $dirKey = ConvertTo-EntryKey $dir
            $present = $false
            foreach ($entry in $entries) {
                if ((ConvertTo-EntryKey $entry) -eq $dirKey) { $present = $true; break }
            }
            if ($present) {
                Write-Host "$dir already in path"
            } else {
                Write-Host "Adding to path: $dir"
                # Keep a trailing separator of the original value in place
                if ($entries.Count -gt 0 -and $entries[$entries.Count - 1].Length -eq 0) {
                    $entries.Insert($entries.Count - 1, $dir)
                } else {
                    $entries.Add($dir)
                }
                $changed = $true
            }
        }
    } else {
        for ($i = $entries.Count - 1; $i -ge 0; $i--) {
            if ($managedKeys -contains (ConvertTo-EntryKey $entries[$i])) {
                Write-Host "Removing from path: $($entries[$i])"
                $entries.RemoveAt($i)
                $changed = $true
            }
        }
    }

    if (-not $changed) {
        Write-Host "No changes needed to PATH"
        exit 0
    }

    $new = [string]::Join(';', $entries)
    if ($new.Trim(';').Trim().Length -eq 0) {
        Write-Host "The new Path would be empty, it is left unchanged."
        exit 1
    }

    # Save the previous value (the key is only writable by administrators)
    $backup = $base.CreateSubKey($BackupKeyPath)
    $backup.SetValue($BackupValueName, $current, [Microsoft.Win32.RegistryValueKind]::String)
    $backup.Close()

    $key.SetValue($ValueName, $new, $kind)
    $key.Close()
    $base.Close()

    if (-not $NoBroadcast) {
        Send-EnvironmentChange
    }
    if ($Action -eq 'add') {
        Write-Host "Successfully added NyxPlay Host directories to PATH"
    } else {
        Write-Host "Successfully removed NyxPlay Host directories from PATH"
    }
    exit 0
} catch {
    Write-Host "Failed to update PATH: $($_.Exception.Message)"
    exit 1
}
