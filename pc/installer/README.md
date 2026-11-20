# NyxPlay for PC setup (NSIS)

`nyxplay-pc.nsi` builds a per-machine Windows setup (64-bit, Windows 10 or later, English and Turkish)
that installs the files of a released portable package unchanged, in normal mode: the portable marker
`portable.dat` and the portable `README.txt` are left out, so settings live in the user profile
(`HKCU\Software\NyxPlay\NyxPlay`, `%LOCALAPPDATA%\NyxPlay\NyxPlay`).

Build (needs NSIS 3 and the release zip):

    powershell -File pc\scripts\build-setup.ps1 -Zip <portable.zip> -Version 2026.11.20.1 -ExpectedSha256 <hash>

Silent use: `setup.exe /S` (defaults: program and Start Menu shortcut), `/D=C:\Folder` (new installs
only, must be the last argument), `/ALLOWDOWNGRADE` (otherwise a silent downgrade is refused).
An existing installation always keeps its folder. To get the uninstaller's exit code, run
`uninstall.exe /S _?=<install folder>`.

Exit codes: 0 success, 1 cancelled, 2 aborted by the script, 3 unsupported Windows, 4 downgrade
refused, 5 NyxPlay is running, 6 install failed, 7 invalid install folder.

The uninstaller removes only the files it installed (plus those listed in `package-files.txt`) and
removes the folder only when it is empty. User settings are kept unless the question is answered Yes
(silent uninstall always keeps them).
