; NyxPlay for PC - Windows setup (NSIS 3, Modern UI 2)
; Installs the files of a released portable package, per machine, in normal (non-portable) mode.
; Build with pc/scripts/build-setup.ps1, which stages the files and generates the two .nsh lists.
;
; Defines expected from the build script:
;   VERSION      dotted version, four numbers (for example 2026.11.20.1)
;   OUTFILE      output path of the setup
;   ICON         path of nyxplay.ico
;   LICENSEFILE  licence text for the licence page (CRLF)
;   FILES_NSH / UNFILES_NSH  generated install and uninstall file lists
;
; Exit codes: 0 success, 1 cancelled by the user, 2 aborted by the setup script,
;   3 unsupported Windows, 4 downgrade refused, 5 NyxPlay is running, 6 install failed,
;   7 invalid install folder.

Unicode true
SetCompressor /SOLID lzma
SetCompressorDictSize 32
RequestExecutionLevel admin
ManifestSupportedOS {8e0f7a12-bfb3-4fe8-b9a5-48fd50a15a9a}
ManifestDPIAware true
CRCCheck on

!define PRODUCT "NyxPlay"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\NyxPlay"
!define URL "https://github.com/siberanka/NyxPlay"

Name "${PRODUCT}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\NyxPlay"
BrandingText "NyxPlay ${VERSION}"

!include MUI2.nsh
!include LogicLib.nsh
!include x64.nsh
!include WinVer.nsh
!include WordFunc.nsh
!include FileFunc.nsh
!include Sections.nsh

VIProductVersion "${VERSION}"
!macro VERSIONINFO LANGID
  VIAddVersionKey /LANG=${LANGID} "ProductName" "NyxPlay"
  VIAddVersionKey /LANG=${LANGID} "FileDescription" "NyxPlay Setup"
  VIAddVersionKey /LANG=${LANGID} "FileVersion" "${VERSION}"
  VIAddVersionKey /LANG=${LANGID} "ProductVersion" "${VERSION}"
  VIAddVersionKey /LANG=${LANGID} "CompanyName" "NyxPlay"
  VIAddVersionKey /LANG=${LANGID} "LegalCopyright" "Copyright NyxPlay contributors. Licensed under the GNU GPL v3 or later."
  VIAddVersionKey /LANG=${LANGID} "Comments" "NyxPlay is free software; see the licence page of this setup."
!macroend

Var ModeText
Var OldVer
Var OldDir
Var InstMode ; 0 new, 1 repair, 2 update, 3 downgrade

!define MUI_ABORTWARNING
!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_LANGDLL_ALWAYSSHOW
!define MUI_LANGDLL_REGISTRY_ROOT HKLM
!define MUI_LANGDLL_REGISTRY_KEY "${UNINST_KEY}"
!define MUI_LANGDLL_REGISTRY_VALUENAME "InstallerLanguage"

!define MUI_WELCOMEPAGE_TEXT "$ModeText"
!define MUI_PAGE_CUSTOMFUNCTION_PRE WelcomePre
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${LICENSEFILE}"
!define MUI_PAGE_CUSTOMFUNCTION_PRE DirPre
!insertmacro MUI_PAGE_DIRECTORY
!define MUI_PAGE_CUSTOMFUNCTION_LEAVE ComponentsLeave
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "$(STR_RunApp)"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchApp
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "English"
!insertmacro MUI_LANGUAGE "Turkish"

!insertmacro VERSIONINFO ${LANG_ENGLISH}
!insertmacro VERSIONINFO ${LANG_TURKISH}

LangString STR_OsError ${LANG_ENGLISH} "NyxPlay requires 64-bit Windows 10 or later."
LangString STR_OsError ${LANG_TURKISH} "NyxPlay, 64 bit Windows 10 veya üstünü gerektirir."
LangString STR_Fresh ${LANG_ENGLISH} "This wizard installs NyxPlay ${VERSION} on your computer.$\r$\n$\r$\nClose other programs before you continue."
LangString STR_Fresh ${LANG_TURKISH} "Bu sihirbaz NyxPlay ${VERSION} sürümünü bilgisayarınıza kurar.$\r$\n$\r$\nDevam etmeden önce diğer programları kapatın."
LangString STR_Repair ${LANG_ENGLISH} "NyxPlay ${VERSION} is already installed in $OldDir.$\r$\n$\r$\nThis wizard will repair / reinstall it. Your settings are kept."
LangString STR_Repair ${LANG_TURKISH} "NyxPlay ${VERSION} zaten $OldDir klasörüne kurulu.$\r$\n$\r$\nBu sihirbaz kurulumu onarır / yeniden kurar. Ayarlarınız korunur."
LangString STR_Update ${LANG_ENGLISH} "Update from $OldVer to ${VERSION}.$\r$\n$\r$\nNyxPlay is installed in $OldDir. Your settings are kept."
LangString STR_Update ${LANG_TURKISH} "$OldVer sürümünden ${VERSION} sürümüne güncelleme.$\r$\n$\r$\nNyxPlay $OldDir klasörüne kurulu. Ayarlarınız korunur."
LangString STR_Downgrade ${LANG_ENGLISH} "A newer version ($OldVer) is already installed. Installing ${VERSION} replaces it with an older version.$\r$\n$\r$\nDo you want to continue?"
LangString STR_Downgrade ${LANG_TURKISH} "Daha yeni bir sürüm ($OldVer) zaten kurulu. ${VERSION} sürümünü kurmak onu eski bir sürümle değiştirir.$\r$\n$\r$\nDevam etmek istiyor musunuz?"
LangString STR_DowngradeText ${LANG_ENGLISH} "Downgrade from $OldVer to ${VERSION}.$\r$\n$\r$\nNyxPlay is installed in $OldDir. Your settings are kept, but an older version may not read settings written by a newer one."
LangString STR_DowngradeText ${LANG_TURKISH} "$OldVer sürümünden ${VERSION} sürümüne geri alma.$\r$\n$\r$\nNyxPlay $OldDir klasörüne kurulu. Ayarlarınız korunur ancak eski bir sürüm, yeni sürümün yazdığı ayarları okuyamayabilir."
LangString STR_Running ${LANG_ENGLISH} "NyxPlay is running. Close NyxPlay (and NyxPlay Updater), then select Retry."
LangString STR_Running ${LANG_TURKISH} "NyxPlay çalışıyor. NyxPlay uygulamasını (ve NyxPlay Updater) kapatıp Yeniden dene seçeneğini seçin."
LangString STR_BadDir ${LANG_ENGLISH} "The install folder is not valid. Choose a folder on a local drive."
LangString STR_BadDir ${LANG_TURKISH} "Kurulum klasörü geçerli değil. Yerel bir sürücüde klasör seçin."
LangString STR_RunApp ${LANG_ENGLISH} "Start NyxPlay"
LangString STR_RunApp ${LANG_TURKISH} "NyxPlay uygulamasını başlat"
LangString STR_SecApp ${LANG_ENGLISH} "NyxPlay (required)"
LangString STR_SecApp ${LANG_TURKISH} "NyxPlay (gerekli)"
LangString STR_SecMenu ${LANG_ENGLISH} "Start Menu shortcut"
LangString STR_SecMenu ${LANG_TURKISH} "Başlat Menüsü kısayolu"
LangString STR_SecDesk ${LANG_ENGLISH} "Desktop shortcut"
LangString STR_SecDesk ${LANG_TURKISH} "Masaüstü kısayolu"
LangString STR_DescApp ${LANG_ENGLISH} "The NyxPlay client program and its libraries."
LangString STR_DescApp ${LANG_TURKISH} "NyxPlay istemci programı ve kitaplıkları."
LangString STR_DescMenu ${LANG_ENGLISH} "Adds NyxPlay to the Start Menu."
LangString STR_DescMenu ${LANG_TURKISH} "NyxPlay uygulamasını Başlat Menüsü'ne ekler."
LangString STR_DescDesk ${LANG_ENGLISH} "Adds a NyxPlay shortcut to the desktop."
LangString STR_DescDesk ${LANG_TURKISH} "Masaüstüne NyxPlay kısayolu ekler."
LangString STR_UnSettings ${LANG_ENGLISH} "Do you also want to delete your NyxPlay settings, paired hosts and caches?$\r$\n$\r$\nChoose No to keep them for a later installation."
LangString STR_UnSettings ${LANG_TURKISH} "NyxPlay ayarlarınızı, eşleştirilmiş host bilgisayarları ve önbellekleri de silmek istiyor musunuz?$\r$\n$\r$\nİleride yeniden kurmak üzere saklamak için Hayır'ı seçin."

; ---------------------------------------------------------------- helpers

; Sets $0 to 1 when NyxPlay.exe or NyxPlayUpdater.exe in $INSTDIR is locked by a running process.
; Only files inside the install folder are probed; nothing is ever terminated.
!macro LOCKCHECK PFX
Function ${PFX}IsRunning
  StrCpy $0 0
  ${If} ${FileExists} "$INSTDIR\NyxPlay.exe"
    ClearErrors
    FileOpen $1 "$INSTDIR\NyxPlay.exe" a
    ${If} ${Errors}
      StrCpy $0 1
    ${Else}
      FileClose $1
    ${EndIf}
  ${EndIf}
  ${If} ${FileExists} "$INSTDIR\NyxPlayUpdater.exe"
    ClearErrors
    FileOpen $1 "$INSTDIR\NyxPlayUpdater.exe" a
    ${If} ${Errors}
      StrCpy $0 1
    ${Else}
      FileClose $1
    ${EndIf}
  ${EndIf}
FunctionEnd
!macroend
!insertmacro LOCKCHECK ""
!insertmacro LOCKCHECK "un."

Function VerifyNotRunning
  StrCpy $2 0
  loop:
    Call IsRunning
    ${If} $0 = 0
      Return
    ${EndIf}
    ${If} ${Silent}
      IntOp $2 $2 + 1
      ${If} $2 > 5
        SetErrorLevel 5
        Quit
      ${EndIf}
      Sleep 2000
      Goto loop
    ${EndIf}
    MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "$(STR_Running)" IDRETRY loop
  SetErrorLevel 5
  Quit
FunctionEnd

; An install folder must be an absolute local path with a drive letter and at least one folder level.
Function ValidateDir
  StrCpy $0 1
  StrLen $1 $INSTDIR
  ${If} $1 < 4
    StrCpy $0 0
    Return
  ${EndIf}
  StrCpy $2 $INSTDIR 1 1
  ${If} $2 != ":"
    StrCpy $0 0
    Return
  ${EndIf}
  StrCpy $2 $INSTDIR 1 2
  ${If} $2 != "\"
    StrCpy $0 0
    Return
  ${EndIf}
  StrCpy $2 $INSTDIR 1 -1
  ${If} $2 == "\"
    StrCpy $0 0
  ${EndIf}
FunctionEnd

Function .onInit
  SetRegView 64
  ${IfNot} ${RunningX64}
  ${OrIfNot} ${AtLeastWin10}
    MessageBox MB_OK|MB_ICONSTOP "$(STR_OsError)" /SD IDOK
    SetErrorLevel 3
    Abort
  ${EndIf}

  !insertmacro MUI_LANGDLL_DISPLAY

  StrCpy $InstMode 0
  StrCpy $OldVer ""
  StrCpy $OldDir ""
  ReadRegStr $OldDir HKLM "${UNINST_KEY}" "InstallLocation"
  ReadRegStr $OldVer HKLM "${UNINST_KEY}" "DisplayVersion"
  ${If} $OldDir != ""
  ${AndIf} ${FileExists} "$OldDir\NyxPlay.exe"
    StrCpy $INSTDIR $OldDir
    StrCpy $InstMode 2
    ${If} $OldVer != ""
      ${VersionCompare} "$OldVer" "${VERSION}" $R0
      ${If} $R0 = 0
        StrCpy $InstMode 1
      ${ElseIf} $R0 = 1
        StrCpy $InstMode 3
      ${EndIf}
    ${EndIf}
  ${EndIf}

  ${If} $InstMode = 3
    ${GetParameters} $R1
    ${GetOptions} $R1 "/ALLOWDOWNGRADE" $R2
    ${If} ${Errors}
    ${AndIf} ${Silent}
      SetErrorLevel 4
      Abort
    ${EndIf}
  ${EndIf}

  Call ValidateDir
  ${If} $0 = 0
    MessageBox MB_OK|MB_ICONSTOP "$(STR_BadDir)" /SD IDOK
    SetErrorLevel 7
    Abort
  ${EndIf}
  ${If} ${Silent}
    Call VerifyNotRunning
  ${EndIf}
FunctionEnd

; The page language is only active once the pages run, so the mode text is built here.
Function WelcomePre
  ${If} $InstMode = 0
    StrCpy $ModeText "$(STR_Fresh)"
  ${ElseIf} $InstMode = 1
    StrCpy $ModeText "$(STR_Repair)"
  ${ElseIf} $InstMode = 2
    StrCpy $ModeText "$(STR_Update)"
  ${Else}
    StrCpy $ModeText "$(STR_DowngradeText)"
    MessageBox MB_YESNO|MB_ICONEXCLAMATION|MB_DEFBUTTON2 "$(STR_Downgrade)" IDYES +3
    SetErrorLevel 4
    Quit
  ${EndIf}
FunctionEnd

Function DirPre
  ; An existing installation keeps its folder.
  ${If} $InstMode != 0
    Abort
  ${EndIf}
FunctionEnd

Function ComponentsLeave
  Call ValidateDir
  ${If} $0 = 0
    MessageBox MB_OK|MB_ICONSTOP "$(STR_BadDir)"
    Abort
  ${EndIf}
  Call VerifyNotRunning
FunctionEnd

Function LaunchApp
  ; Start through the shell so the client runs with the user's normal token, not the elevated one.
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\NyxPlay.exe"'
FunctionEnd

Function .onInstFailed
  SetErrorLevel 6
FunctionEnd

; ---------------------------------------------------------------- sections

Section "!$(STR_SecApp)" SecApp
  SectionIn RO
  SetShellVarContext all
  SetRegView 64
  Call VerifyNotRunning
  SetOverwrite on
  !include "${FILES_NSH}"
  SetOutPath "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  WriteRegStr HKLM "${UNINST_KEY}" "DisplayName" "NyxPlay"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "${UNINST_KEY}" "Publisher" "NyxPlay"
  WriteRegStr HKLM "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\NyxPlay.exe,0"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKLM "${UNINST_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKLM "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegStr HKLM "${UNINST_KEY}" "URLInfoAbout" "${URL}"
  WriteRegStr HKLM "${UNINST_KEY}" "HelpLink" "${URL}"
  WriteRegStr HKLM "${UNINST_KEY}" "InstallerLanguage" "$LANGUAGE"
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  WriteRegDWORD HKLM "${UNINST_KEY}" "EstimatedSize" $0
SectionEnd

Section "$(STR_SecMenu)" SecMenu
  SetShellVarContext all
  SetOutPath "$INSTDIR"
  CreateShortcut "$SMPROGRAMS\NyxPlay.lnk" "$INSTDIR\NyxPlay.exe" "" "$INSTDIR\NyxPlay.exe" 0
SectionEnd

Section /o "$(STR_SecDesk)" SecDesk
  SetShellVarContext all
  SetOutPath "$INSTDIR"
  CreateShortcut "$DESKTOP\NyxPlay.lnk" "$INSTDIR\NyxPlay.exe" "" "$INSTDIR\NyxPlay.exe" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecApp} "$(STR_DescApp)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMenu} "$(STR_DescMenu)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesk} "$(STR_DescDesk)"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------- uninstaller

Function un.onInit
  SetRegView 64
  !insertmacro MUI_UNGETLANGUAGE
FunctionEnd

; Deletes the relative paths listed in package-files.txt (covers files an in-place update added).
Function un.DeleteListed
  ${IfNot} ${FileExists} "$INSTDIR\package-files.txt"
    Return
  ${EndIf}
  FileOpen $3 "$INSTDIR\package-files.txt" r
  next:
    ClearErrors
    FileRead $3 $4
    ${If} ${Errors}
      FileClose $3
      Return
    ${EndIf}
    ${WordReplace} "$4" "$\r" "" "+" $4
    ${WordReplace} "$4" "$\n" "" "+" $4
    ${WordReplace} "$4" "/" "\" "+" $4
    StrCmp $4 "" next
    ; reject anything that could leave the install folder
    StrCpy $5 $4 1
    StrCmp $5 "\" next
    StrCpy $5 $4 1 1
    StrCmp $5 ":" next
    ${WordFind} "$4" ".." "E+1" $5
    ${IfNot} ${Errors}
      Goto next
    ${EndIf}
    Delete "$INSTDIR\$4"
  Goto next
FunctionEnd

Section "Uninstall"
  SetShellVarContext all
  SetRegView 64
  ; refuse to run from anywhere but a folder that holds the NyxPlay program
  ${IfNot} ${FileExists} "$INSTDIR\NyxPlay.exe"
    SetErrorLevel 7
    Abort
  ${EndIf}
  retry:
    Call un.IsRunning
    ${If} $0 = 1
      ${If} ${Silent}
        SetErrorLevel 5
        Abort
      ${EndIf}
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "$(STR_Running)" IDRETRY retry
      SetErrorLevel 5
      Abort
    ${EndIf}

  Delete "$SMPROGRAMS\NyxPlay.lnk"
  Delete "$DESKTOP\NyxPlay.lnk"

  Call un.DeleteListed
  !include "${UNFILES_NSH}"
  Delete "$INSTDIR\uninstall.exe"
  ; non-recursive: only an empty folder is removed, anything else in it is left alone
  RMDir "$INSTDIR\.nyxupdate"
  RMDir "$INSTDIR"

  DeleteRegKey HKLM "${UNINST_KEY}"

  ${IfNot} ${Silent}
    MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "$(STR_UnSettings)" IDNO keep
    SetShellVarContext current
    DeleteRegKey HKCU "Software\NyxPlay\NyxPlay"
    DeleteRegKey /ifempty HKCU "Software\NyxPlay"
    RMDir /r "$LOCALAPPDATA\NyxPlay\NyxPlay"
    RMDir "$LOCALAPPDATA\NyxPlay"
    RMDir /r "$APPDATA\NyxPlay\NyxPlay"
    RMDir "$APPDATA\NyxPlay"
    keep:
  ${EndIf}
SectionEnd
